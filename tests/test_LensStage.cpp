#include "Lens.h"
#include "ProcessingPlan.h"
#include "RenderCheckpoint.h"
#include "StageTable.h"
#include "support/Fixtures.h"
#include "support/LadderTesting.h"
#include "support/LensFixtures.h"
#include "support/Lensfun.h"
#include "support/TempDir.h"
#include "support/TestImages.h"

#include <CheckpointLadder.h>
#include <Develop.h>
#include <DevelopState.h>
#include <Diagnostics.h>
#include <ImageImport.h>
#include <LensResolution.h>
#include <NegativeSettings.h>
#include <NoiseReductionSettings.h>
#include <Photo.h>
#include <Progress.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// The Lens stage in the render (ADR 050, section 2 and 6): the plan block, the rung, the
/// resolution of a photograph's lens, and the notices of what cannot be resolved.

namespace {

/// @brief Makes a barrel distortion by hand, with its fill scale set, for a frame of an aspect.
LensCorrection barrel(float strength, double aspect = 1.6) {
    LensCorrection correction;
    correction.aspect = aspect;
    correction.distortion.resize(lensTableNodes);
    for (std::size_t node = 0; node < lensTableNodes; ++node) {
        const double radius = static_cast<double>(node) / (lensTableNodes - 1);
        correction.distortion[node] = static_cast<float>(1.0 + strength * radius * radius);
    }
    return withFillScale(std::move(correction));
}

/// @brief Makes a vignetting profile by hand for a frame of an aspect.
LensCorrection falloff(double aspect = 1.6) {
    LensCorrection correction;
    correction.aspect = aspect;
    correction.vignetting.resize(lensTableNodes);
    for (std::size_t node = 0; node < lensTableNodes; ++node) {
        const double radius = static_cast<double>(node) / (lensTableNodes - 1);
        correction.vignetting[node] = static_cast<float>(1.0 + 0.8 * radius * radius);
    }
    return withFillScale(std::move(correction));
}

/// @brief Makes the source the tests render.
std::shared_ptr<const ImageBuffer> makeSource() {
    return std::make_shared<const ImageBuffer>(
        rainbow({160, 100}, PixelFormat::RgbaF32, workingEncoding));
}

/// @brief A state with colour noise reduction on, so that a Denoise rung exists.
DevelopState denoisedState(float exposure = 0.0F) {
    return stateWith(exposure, 0.0, 0.0F, 40.0F);
}

} // namespace

TEST_CASE("With no correction the Lens stage is no stage", "[lens][stage]") {
    const auto source = makeSource();
    const DevelopState state = denoisedState();

    const ProcessingPlan plan = planFor(*source, state, LensCorrection{});
    CHECK_FALSE(plan.lens.active());
    CHECK_FALSE(rowOf(Stage::Lens).runs(plan));
    CHECK_FALSE(keepsRung(Stage::Lens, plan));
    CHECK(plan.lens == LensPlan{});

    // A correction with no table is none either.
    LensCorrection empty;
    empty.aspect = 1.6;
    CHECK_FALSE(planFor(*source, state, empty).lens.active());

    // Its boundary is the one before it, bit for bit.
    CHECK(sameBits(developUntil(*source, state, LensCorrection{}, Stage::Lens).readBack(),
                   developUntil(*source, state, LensCorrection{}, Stage::Denoise).readBack()));
    // And a ladder keeps no rung there.
    CheckpointLadder ladder;
    static_cast<void>(resumeOrDevelop(ladder, source, state, LensCorrection{}, fitting(40)));
    CHECK(ladder.holds(Stage::Denoise));
    CHECK_FALSE(ladder.holds(Stage::Lens));
    CHECK(ladder.holds(Stage::Pointwise));
}

TEST_CASE("Stopping after the Lens stage gives the corrected frame", "[lens][stage]") {
    const auto source = makeSource();
    const DevelopState state = denoisedState(0.3F);
    const LensCorrection lens = barrel(0.08F);
    REQUIRE(lens.fill > 1.0);

    const RenderCheckpoint denoised = developUntil(*source, state, lens, Stage::Denoise);
    const RenderCheckpoint corrected = developUntil(*source, state, lens, Stage::Lens);
    CHECK(corrected.boundary() == Stage::Lens);
    CHECK(corrected.size() == source->size());
    CHECK(corrected.encoding() == source->encoding());

    // It is the pass applied to the pixels after noise reduction, in the sensor's orientation.
    const ImageBuffer expected = applyLens(denoised.readBack(), LensPlan{lens});
    CHECK(sameBits(corrected.readBack(), expected));
    CHECK_FALSE(sameBits(corrected.readBack(), denoised.readBack()));
    CHECK(corrected.readBack().orientation() == source->orientation());

    // The rest of the render carries on from it, to the whole render.
    const ImageBuffer whole = develop(*source, state, lens);
    CHECK(sameBits(resumeFrom(corrected, *source, state, lens, Stage::Effects).readBack(), whole));
    CHECK_FALSE(sameBits(whole, develop(*source, state, LensCorrection{})));

    // A correction of another strength is another render.
    CHECK_FALSE(sameBits(whole, develop(*source, state, barrel(-0.04F))));
}

TEST_CASE("A lens toggle resumes from the Denoise rung and a develop edit from the Lens rung",
          "[lens][stage][ladder]") {
    const auto source = makeSource();
    const RenderRequest request = fitting(80);
    const LensCorrection lens = barrel(-0.06F);
    DevelopState state = denoisedState();
    CheckpointLadder ladder;

    const auto render = [&](const DevelopState& at, const LensCorrection& with) {
        const LadderRender rendered = resumeOrDevelop(ladder, source, at, with, request);
        CHECK(sameBits(rendered.checkpoint.readBack(), develop(*source, at, with, request)));
        return rendered.resumedFrom;
    };

    CHECK_FALSE(render(state, LensCorrection{}).has_value());
    // Turned on: noise reduction is not run again, and nothing is decoded.
    CHECK(render(state, lens) == Stage::Denoise);
    CHECK(ladder.holds(Stage::Lens));
    // A develop edit resumes after the Lens stage, which is not run again.
    state.settings.tone.exposure = 0.5F;
    CHECK(render(state, lens) == Stage::Lens);
    // A geometry edit resumes after the chain, which is not run again either.
    state.settings.geometry.straighten = 1.0;
    CHECK(render(state, lens) == Stage::Pointwise);
    // Another correction is another Lens rung, from the same Denoise rung.
    CHECK(render(state, barrel(-0.03F)) == Stage::Denoise);
    // Turned off again.
    CHECK(render(state, LensCorrection{}) == Stage::Denoise);
    CHECK_FALSE(ladder.holds(Stage::Lens));
    // A noise reduction edit runs everything after the source again.
    state.settings.noiseReduction.color = 10.0F;
    CHECK_FALSE(render(state, lens).has_value());
}

TEST_CASE("A vignetting amount resumes from the Denoise rung, up from 0 and down from 100",
          "[lens][stage][ladder]") {
    const auto source = makeSource();
    const RenderRequest request = fitting(80);
    const LensCorrection profile = falloff();
    const LensCorrection sixty = withVignettingAmount(profile, 60.0F);
    REQUIRE(sixty != profile);
    const DevelopState state = denoisedState();

    const auto render = [&](CheckpointLadder& ladder, const LensCorrection& with) {
        const LadderRender rendered = resumeOrDevelop(ladder, source, state, with, request);
        CHECK(sameBits(rendered.checkpoint.readBack(), develop(*source, state, with, request)));
        return rendered.resumedFrom;
    };

    CheckpointLadder atProfile;
    CHECK_FALSE(render(atProfile, profile).has_value());
    CHECK(render(atProfile, sixty) == Stage::Denoise);
    CHECK(render(atProfile, withVignettingAmount(profile, 0.0F)) == Stage::Denoise);

    CheckpointLadder fromZero;
    CHECK_FALSE(render(fromZero, LensCorrection{}).has_value());
    CHECK(render(fromZero, sixty) == Stage::Denoise);
    // Plans of other amounts differ from the Lens boundary on, and agree before it.
    const ProcessingPlan a = planFor(*source, state, profile);
    const ProcessingPlan b = planFor(*source, state, sixty);
    CHECK(prefixMatches(a, b, Stage::Denoise));
    CHECK_FALSE(prefixMatches(a, b, Stage::Lens));
}

TEST_CASE("Without noise reduction a toggle resumes from the source, which is held",
          "[lens][stage][ladder]") {
    const auto source = makeSource();
    const DevelopState state = stateWith(0.0F, 0.0);
    REQUIRE_FALSE(reducesNoise(state.settings.noiseReduction));
    CheckpointLadder ladder;
    static_cast<void>(resumeOrDevelop(ladder, source, state, LensCorrection{}, fitting(80)));

    const LensCorrection lens = barrel(0.05F);
    const LadderRender on = resumeOrDevelop(ladder, source, state, lens, fitting(80));
    CHECK_FALSE(on.resumedFrom.has_value());
    CHECK(ladder.holds(Stage::Lens));
    DevelopState edited = state;
    edited.settings.tone.exposure = 0.4F;
    CHECK(resumeOrDevelop(ladder, source, edited, lens, fitting(80)).resumedFrom == Stage::Lens);
}

TEST_CASE("Plans that differ only in the Lens block are stale by value from the Lens boundary on",
          "[lens][stage][plan]") {
    const auto source = makeSource();
    const DevelopState state = denoisedState();
    const ProcessingPlan off = planFor(*source, state, LensCorrection{});
    const ProcessingPlan on = planFor(*source, state, barrel(-0.06F));
    const ProcessingPlan other = planFor(*source, state, barrel(-0.03F));

    for (const ProcessingPlan* changed : {&on, &other}) {
        CHECK(prefixMatches(off, *changed, Stage::Denoise));
        for (const Stage boundary :
             {Stage::Lens, Stage::Pointwise, Stage::Geometry, Stage::Resize, Stage::Effects}) {
            CHECK_FALSE(prefixMatches(off, *changed, boundary));
        }
        CHECK_FALSE(sameAtTap(off, *changed, Tap::CurveInput));
    }
    CHECK(prefixMatches(on, on, Stage::Effects));
    CHECK(prefixMatches(on, other, Stage::Denoise));
    CHECK_FALSE(prefixMatches(on, other, Stage::Lens));

    // The block holds the tables and no size, so another level of the same photograph shares it.
    const ImageBuffer small = rainbow({80, 50}, PixelFormat::RgbaF32, workingEncoding);
    CHECK(planFor(small, state, barrel(-0.06F)).lens == on.lens);

    // A develop edit leaves the block alone.
    DevelopState edited = state;
    edited.settings.tone.exposure = 1.0F;
    CHECK(planFor(*source, edited, barrel(-0.06F)).lens == on.lens);
}

TEST_CASE("The Lens pass is reported, and a render through it stays well formed",
          "[lens][stage][progress]") {
    const auto source = makeSource();
    const DevelopState state = denoisedState();

    ReportLog log;
    ProgressChannel channel(log.callback());
    static_cast<void>(develop(*source, state, barrel(-0.06F), fitting(80), &channel));
    CHECK(log.wellFormed());
    CHECK(std::ranges::any_of(
        log.reports, [](const Progress& progress) { return progress.step == ProgressStep::Lens; }));

    ReportLog quiet;
    ProgressChannel quietChannel(quiet.callback());
    static_cast<void>(develop(*source, state, LensCorrection{}, fitting(80), &quietChannel));
    CHECK(quiet.wellFormed());
    CHECK(std::ranges::none_of(quiet.reports, [](const Progress& progress) {
        return progress.step == ProgressStep::Lens;
    }));

    // A resume from the Lens rung starts after it.
    const RenderCheckpoint rung = developUntil(*source, state, barrel(-0.06F), Stage::Lens);
    ReportLog resumed;
    ProgressChannel resumedChannel(resumed.callback());
    static_cast<void>(resumeFrom(rung, *source, state, barrel(-0.06F), Stage::Effects, fitting(80),
                                 &resumedChannel));
    CHECK(resumed.wellFormed());
    CHECK(std::ranges::none_of(resumed.reports, [](const Progress& progress) {
        return progress.step == ProgressStep::Lens;
    }));
}

TEST_CASE("The plan of a photograph resolves its lens from the correction given", "[lens][stage]") {
    const Photo photo = openPhoto(fixture("lens-canon-32x24.dng"));
    const LensCorrection lens = barrel(-0.05F, 32.0 / 24.0);
    const ProcessingPlan plan = planFor(photo, lens);
    CHECK(plan.lens == LensPlan{lens});
    CHECK(plan.decode == decodeOptionsFor(photo, false));
    // With every switch off, the plan resolves it alone and needs no database.
    CHECK_FALSE(planFor(photo).lens.active());

    // A lens edit changes the Lens block and nothing of the decode; a develop edit changes neither.
    NegativeSettings negative = photo.negative();
    negative.lens.distortion = true;
    CHECK(planFor(photo.with(negative), lens).decode == plan.decode);
    DevelopState edited = photo.state();
    edited.settings.tone.exposure = 1.0F;
    const ProcessingPlan developed = planFor(photo.with(edited), lens);
    CHECK(developed.decode == plan.decode);
    CHECK(developed.lens == plan.lens);
}

namespace {

/// @brief The photograph of the lens fixture, with other facts about its lens and the switches.
Photo photoWith(LensFacts facts, LensSettings settings) {
    const Photo base = openPhoto(fixture("lens-canon-32x24.dng"));
    ImageMetadata metadata = base.metadata();
    metadata.lens = std::move(facts);
    NegativeSettings negative = base.negative();
    negative.lens = std::move(settings);
    return Photo(base.path(), std::move(metadata), negative, base.state());
}

/// @brief Facts of a shot on the reference body.
LensFacts on80D(std::vector<std::string> names) {
    LensFacts facts;
    facts.cameraMaker = "Canon";
    facts.cameraModel = "Canon EOS 80D";
    facts.lensNames = std::move(names);
    return facts;
}

/// @brief The kit lens at 50 mm, as the fixture says.
LensFacts kitFacts() {
    LensFacts facts = on80D({"EF-S18-135mm f/3.5-5.6 IS USM"});
    facts.focalLength = 50.0;
    facts.aperture = 5.6;
    return facts;
}

const LensSettings distortionOnly{.distortion = true,
                                  .vignettingAmount = minimumVignettingAmount,
                                  .lateralCa = false,
                                  .lens = std::nullopt};
const LensSettings allOn{.distortion = true,
                         .vignettingAmount = profileVignettingAmount,
                         .lateralCa = true,
                         .lens = std::nullopt};

/// @brief Copies the fixture database into a directory, replacing a text in its lens file and
/// writing another stamp.
void copyDatabase(const test::TempDir& directory, const std::string& from, const std::string& to,
                  const std::string& stamp) {
    std::ifstream file(fixture("lensfun") / "canon-mini.xml");
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    for (auto at = text.find(from); !from.empty() && at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    std::ofstream(directory.file("canon-mini.xml")) << text;
    std::ofstream(directory.file("timestamp.txt")) << stamp << "\n";
}

/// @brief Resolves a photograph's corrections and keeps what was said.
struct Resolved {
    CollectedDiagnostics log;
    LensCorrection correction;

    Resolved(const Photo& photo, const LensDatabase& database)
        : correction(lensCorrectionFor(photo, database, log)) {}
};

Resolved resolve(const Photo& photo, const LensDatabase& database) {
    return {photo, database};
}

/// @brief Requires exactly one notice, and gives it.
Diagnostic only(const CollectedDiagnostics& log) {
    REQUIRE(log.entries().size() == 1);
    return log.entries().front();
}

} // namespace

TEST_CASE("A matched lens resolves into tables, and says where they came from",
          "[lensfun][lens][stage]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const Photo photo = photoWith(kitFacts(), allOn);
    const Resolved resolved = resolve(photo, test::fixtureDatabase());
    CHECK(resolved.log.entries().empty());
    REQUIRE(resolved.correction.active());
    CHECK(resolved.correction.fill > 1.0);
    REQUIRE(resolved.correction.distortionFrom);
    CHECK(resolved.correction.distortionFrom->lens.model == "Canon EF-S 18-135mm f/3.5-5.6 IS USM");
    CHECK(resolved.correction.distortionFrom->focalLengthRead);
    CHECK(resolved.correction.distortionFrom->cropFactor == 1.6);
    CHECK(resolved.correction.distortionFrom->database == test::fixtureDatabase().stamp());
    CHECK(resolved.correction.aspect == 32.0 / 24.0);

    // Only what is switched on is sampled.
    const Resolved distortion =
        resolve(photoWith(kitFacts(), distortionOnly), test::fixtureDatabase());
    CHECK_FALSE(distortion.correction.distortion.empty());
    CHECK(distortion.correction.vignetting.empty());
    CHECK_FALSE(distortion.correction.hasLateralCa());

    // With nothing switched on the database is not even looked at.
    const test::TempDir empty;
    const Resolved off =
        resolve(photoWith(kitFacts(), LensSettings{}), LensDatabase::fromDirectory(empty.path()));
    CHECK_FALSE(off.correction.active());
    CHECK(off.log.entries().empty());
}

TEST_CASE("A recorded lens is used by its name, whatever would be matched now",
          "[lensfun][lens][stage]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const Photo matched = photoWith(kitFacts(), allOn);

    // Recorded from the match, it resolves to the same tables as matching does.
    const Photo recorded = recordLensMatch(matched, database);
    REQUIRE(recorded.negative().lens.lens);
    CHECK(resolve(recorded, database).correction == resolve(matched, database).correction);
    CHECK(resolve(recorded, database).log.entries().empty());

    // A lens the photographer chose is the one used, though the file names another.
    const Photo chosen = *withChosenLens(matched, {"Canon", "Canon EF 50mm f/1.8 STM"}, database);
    CHECK(chosen.negative().lens.lens->origin == LensOrigin::Chosen);
    CHECK(chosen.negative().lens.lens->cropFactor == 1.6);
    const Resolved resolved = resolve(chosen, database);
    REQUIRE(resolved.correction.distortionFrom);
    CHECK(resolved.correction.distortionFrom->lens.model == "Canon EF 50mm f/1.8 STM");
    CHECK_FALSE(resolved.correction == resolve(matched, database).correction);

    // A lens that is not listed cannot be chosen, nor can one for a photograph that is no RAW.
    CHECK_FALSE(withChosenLens(matched, {"Canon", "Canon EF 85mm f/1.2L II USM"}, database));
    CHECK_FALSE(withChosenLens(openPhoto(fixture("testcard-61x41-srgb8.png")),
                               {"Canon", "Canon EF 50mm f/1.8 STM"}, database));
}

TEST_CASE("A recorded lens that is gone is not matched again, and a new stamp is told",
          "[lensfun][lens][stage]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const Photo recorded = recordLensMatch(photoWith(kitFacts(), allOn), database);
    REQUIRE(recorded.negative().lens.lens);

    SECTION("renamed in the database") {
        // The facts would now match the renamed lens by its name; the record is not consulted
        // for that, so nothing is applied.
        const test::TempDir directory;
        copyDatabase(directory, "Canon EF-S 18-135mm f/3.5-5.6 IS USM",
                     "Canon EF-S 18-135mm f/3.5-5.6 IS USM II", "1700000000");
        const LensDatabase renamed = LensDatabase::fromDirectory(directory.path());
        const Resolved resolved = resolve(recorded, renamed);
        CHECK_FALSE(resolved.correction.active());
        const Diagnostic note = only(resolved.log);
        CHECK(note.notice == Notice::LensProfileMissing);
        CHECK(note.severity == Severity::Warning);
        CHECK(note.subject == recorded.path());
        CHECK(std::get<std::string>(note.values.at(0)) == "Canon EF-S 18-135mm f/3.5-5.6 IS USM");
        // Matched afresh, without a record, the same facts do find a lens in that database.
        CHECK(resolve(photoWith(kitFacts(), allOn), renamed).correction.active());
    }
    SECTION("the same lens in a database of another stamp") {
        const test::TempDir directory;
        copyDatabase(directory, "", "", "1800000000");
        const LensDatabase newer = LensDatabase::fromDirectory(directory.path());
        REQUIRE(newer.stamp() != database.stamp());
        const Resolved resolved = resolve(recorded, newer);
        // Still applied: the lens is the same, though its calibration may have been revised.
        CHECK(resolved.correction.active());
        const Diagnostic note = only(resolved.log);
        CHECK(note.notice == Notice::LensDatabaseChanged);
        CHECK(note.severity == Severity::Info);
        CHECK(std::get<std::string>(note.values.at(0)) == database.stamp());
        CHECK(std::get<std::string>(note.values.at(1)) == newer.stamp());
        CHECK(resolved.correction.distortionFrom->database == newer.stamp());
    }
    SECTION("a zoom whose focal length the file does not say") {
        LensFacts facts = kitFacts();
        facts.focalLength.reset();
        const Photo photo = photoWith(facts, recorded.negative().lens);
        const Resolved resolved = resolve(photo, database);
        CHECK_FALSE(resolved.correction.active());
        CHECK(only(resolved.log).notice == Notice::LensFocalUnknown);
    }
}

TEST_CASE("A correction that is on and cannot be resolved is skipped with one notice that says why",
          "[lensfun][lens][stage]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();

    const auto refused = [&](LensFacts facts, Notice expected) {
        const Resolved resolved = resolve(photoWith(std::move(facts), allOn), database);
        CHECK_FALSE(resolved.correction.active());
        const Diagnostic note = only(resolved.log);
        CHECK(note.notice == expected);
        CHECK(note.severity == Severity::Warning);
        CHECK(note.subject.has_value());
        CHECK_FALSE(describe(note).empty());
        return note;
    };

    SECTION("no lens named") {
        refused(on80D({}), Notice::LensUnidentified);
    }
    SECTION("a lens the database does not list") {
        const Diagnostic note =
            refused(on80D({"Canon EF 85mm f/1.2L II USM"}), Notice::LensNotInDatabase);
        CHECK(std::get<std::string>(note.values.at(0)) == "Canon EF 85mm f/1.2L II USM");
    }
    SECTION("a zoom used at an unrecorded focal length") {
        const Diagnostic note =
            refused(on80D({"EF-S18-135mm f/3.5-5.6 IS USM"}), Notice::LensFocalUnknown);
        CHECK(std::get<std::string>(note.values.at(0)) == "Canon EF-S 18-135mm f/3.5-5.6 IS USM");
    }
    SECTION("two lenses that fit equally well") {
        LensFacts facts = on80D({"Sigma 17-50mm f/2.8"});
        facts.minFocal = 17.0;
        facts.maxFocal = 50.0;
        facts.maxAperture = 2.8;
        facts.focalLength = 24.0;
        const Diagnostic note = refused(facts, Notice::LensAmbiguous);
        CHECK_THAT(describe(note), Catch::Matchers::ContainsSubstring("Sigma 17-50mm f/2.8 EX DC"));
    }
    SECTION("a database that did not load") {
        const test::TempDir empty;
        const LensDatabase missing = LensDatabase::fromDirectory(empty.path());
        const Resolved resolved = resolve(photoWith(kitFacts(), allOn), missing);
        CHECK_FALSE(resolved.correction.active());
        CHECK(only(resolved.log).notice == Notice::LensDatabaseMissing);
    }
}

TEST_CASE("A lens without a calibration for a correction is told, and the others still apply",
          "[lensfun][lens][stage]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts = on80D({"Sigma 18-35mm F1.8 DC HSM"});
    facts.minFocal = 18.0;
    facts.maxFocal = 35.0;
    facts.maxAperture = 1.8;
    facts.focalLength = 24.0;
    facts.aperture = 2.8;
    const Resolved resolved = resolve(photoWith(facts, allOn), test::fixtureDatabase());
    CHECK_FALSE(resolved.correction.distortion.empty());
    CHECK(resolved.correction.vignetting.empty());
    const Diagnostic note = only(resolved.log);
    CHECK(note.notice == Notice::LensNotCalibrated);
    CHECK(note.severity == Severity::Warning);
    CHECK(std::get<std::string>(note.values.at(1)) == "vignetting, chromatic aberration");
}

TEST_CASE("A camera that is not listed is told, and the lens is still found",
          "[lensfun][lens][stage]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensFacts facts;
    facts.cameraMaker = "Mystery";
    facts.cameraModel = "Unlisted 1";
    facts.lensNames = {"Canon EF 50mm f/1.8 STM"};
    facts.focalLength = 50.0;
    facts.focalLength35 = 80.0;
    const Resolved resolved = resolve(photoWith(facts, distortionOnly), test::fixtureDatabase());
    CHECK(resolved.correction.active());
    const Diagnostic note = only(resolved.log);
    CHECK(note.notice == Notice::CameraNotInDatabase);
    CHECK(note.severity == Severity::Info);
    CHECK(resolved.correction.distortionFrom->cropFactor == 1.6);
}

TEST_CASE("Lens corrections are for a RAW, and a photograph that is not one says nothing",
          "[lens][stage]") {
    const test::TempDir empty;
    const LensDatabase database = LensDatabase::fromDirectory(empty.path());
    NegativeSettings negative;
    negative.lens = allOn;
    const Photo png = openPhoto(fixture("testcard-61x41-srgb8.png")).with(negative);
    const Resolved resolved = resolve(png, database);
    CHECK_FALSE(resolved.correction.active());
    CHECK(resolved.log.entries().empty());
    // Not even the process's database is wanted for one.
    CollectedDiagnostics log;
    CHECK_FALSE(lensCorrectionFor(png, log).active());
    CHECK(log.entries().empty());
}

TEST_CASE("A build without lensfun says so rather than rendering as if there were no profile",
          "[lens][stage]") {
    if (lensfunAvailable()) {
        SUCCEED("this build has lensfun");
        return;
    }
    const test::TempDir empty;
    const Resolved resolved =
        resolve(photoWith(kitFacts(), allOn), LensDatabase::fromDirectory(empty.path()));
    CHECK_FALSE(resolved.correction.active());
    CHECK(only(resolved.log).notice == Notice::LensfunUnavailable);
    // And the render goes ahead without it.
    const auto source = makeSource();
    CHECK(sameBits(develop(*source, denoisedState(), resolved.correction),
                   develop(*source, denoisedState(), LensCorrection{})));
}

TEST_CASE("Every problem has a notice of its name", "[lens]") {
    CHECK(noticeFor(LensProblem::LensUnidentified) == Notice::LensUnidentified);
    CHECK(noticeFor(LensProblem::LensFocalUnknown) == Notice::LensFocalUnknown);
    CHECK(noticeFor(LensProblem::CameraNotInDatabase) == Notice::CameraNotInDatabase);
    CHECK(noticeFor(LensProblem::LensNotInDatabase) == Notice::LensNotInDatabase);
    CHECK(noticeFor(LensProblem::LensAmbiguous) == Notice::LensAmbiguous);
    CHECK(noticeFor(LensProblem::LensProfileMissing) == Notice::LensProfileMissing);
    CHECK(noticeFor(LensProblem::LensDatabaseChanged) == Notice::LensDatabaseChanged);
    CHECK(noticeFor(LensProblem::LensDatabaseMissing) == Notice::LensDatabaseMissing);
    CHECK(noticeFor(LensProblem::LensfunUnavailable) == Notice::LensfunUnavailable);
}
