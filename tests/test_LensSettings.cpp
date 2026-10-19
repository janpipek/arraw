#include "support/Fixtures.h"
#include "support/LensFixtures.h"
#include "support/Lensfun.h"
#include "support/TempDir.h"

#include <DecodeOptions.h>
#include <Diagnostics.h>
#include <EditSession.h>
#include <Edits.h>
#include <ImageImport.h>
#include <LensResolution.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <SettingsJson.h>
#include <Sidecar.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

/// The lens switches and the lens recorded for them, as settings of the negative (ADR 050,
/// section 1 and 8): the type, its persistence, its history and its copying. No pixels.

namespace {

constexpr const char* lensFixture = "lens-canon-32x24.dng";

/// @brief Gives a photograph with the switches set.
Photo withSwitches(const Photo& photo, bool distortion, bool vignetting = false,
                   bool lateralCa = false) {
    NegativeSettings negative = photo.negative();
    negative.lens.distortion = distortion;
    negative.lens.vignettingAmount = vignetting ? profileVignettingAmount : minimumVignettingAmount;
    negative.lens.lateralCa = lateralCa;
    return photo.with(negative);
}

/// @brief Copies the lens fixture into a directory and opens it from there, so it can have a
/// sidecar.
Photo lensPhotoIn(const test::TempDir& directory, const char* name = "frame.dng") {
    const auto path = directory.file(name);
    std::filesystem::copy_file(test::fixture(lensFixture), path);
    return openPhoto(path);
}

/// @brief Reads a whole file.
std::string textOf(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// @brief Replaces the first occurrence of a text in a file.
void replaceInFile(const std::filesystem::path& path, const std::string& from,
                   const std::string& to) {
    std::string text = textOf(path);
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    std::ofstream(path) << text;
}

/// @brief A recorded lens, as matching the kit lens on the reference body would record it.
RecordedLens kitLens(std::string database = "2023-11-14T22:13:20Z") {
    return {.maker = "Canon",
            .model = "Canon EF-S 18-135mm f/3.5-5.6 IS USM",
            .cropFactor = 1.6,
            .origin = LensOrigin::Matched,
            .database = std::move(database)};
}

} // namespace

TEST_CASE("Every lens origin round-trips through its token", "[lens][negative]") {
    REQUIRE(lensOriginTokens.size() == lensOriginCount);
    for (std::size_t index = 0; index < lensOriginCount; ++index) {
        const auto origin = static_cast<LensOrigin>(index);
        REQUIRE(tokenOf(origin) == lensOriginTokens[index]);
        REQUIRE(parseLensOrigin(lensOriginTokens[index]) == origin);
    }
    REQUIRE(tokenOf(LensOrigin::Matched) == "matched");
    REQUIRE(tokenOf(LensOrigin::Chosen) == "chosen");
    REQUIRE(tokenOf(static_cast<LensOrigin>(9)).empty());
    REQUIRE_FALSE(parseLensOrigin("Matched"));
    REQUIRE_FALSE(parseLensOrigin("automatic"));
}

TEST_CASE("Corrections are off for every photograph, and nothing is recorded", "[lens][negative]") {
    const LensSettings settings;
    CHECK_FALSE(settings.distortion);
    CHECK(settings.vignettingAmount == 0.0F);
    CHECK_FALSE(settings.lateralCa);
    CHECK_FALSE(settings.lens.has_value());
    CHECK_FALSE(settings.any());

    for (const char* name : {lensFixture, "testcard-61x41-srgb8.png"}) {
        CAPTURE(name);
        const Photo photo = openPhoto(test::fixture(name));
        CHECK(photo.negative().lens == LensSettings{});
        CHECK(defaultNegativeFor(photo.metadata()).lens == LensSettings{});
    }
}

TEST_CASE("Validation refuses a recorded lens that cannot name a lens", "[lens][negative]") {
    NegativeSettings negative;
    negative.lens.lens = kitLens();
    REQUIRE_NOTHROW(validate(negative));

    negative.lens.lens->model.clear();
    REQUIRE_THROWS_AS(validate(negative), std::invalid_argument);
    negative.lens.lens = kitLens();
    negative.lens.lens->cropFactor = 0.0;
    REQUIRE_THROWS_AS(validate(negative), std::invalid_argument);
    negative.lens.lens->cropFactor = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS_AS(validate(negative), std::invalid_argument);
    negative.lens.lens = kitLens();
    negative.lens.lens->origin = static_cast<LensOrigin>(7);
    REQUIRE_THROWS_AS(validate(negative), std::invalid_argument);

    // The maker may be empty: lensfun lists some lenses without one.
    negative.lens.lens = kitLens();
    negative.lens.lens->maker.clear();
    REQUIRE_NOTHROW(validate(negative));
}

TEST_CASE("A lens edit never changes what is decoded, and a develop edit never changes a decode",
          "[lens][negative][decode]") {
    const Photo photo = openPhoto(test::fixture(lensFixture));
    for (const bool halfSize : {false, true}) {
        const DecodeOptions before = decodeOptionsFor(photo, halfSize);

        NegativeSettings negative = photo.negative();
        negative.lens = {.distortion = true,
                         .vignettingAmount = profileVignettingAmount,
                         .lateralCa = true,
                         .lens = kitLens()};
        const Photo corrected = photo.with(negative);
        CHECK(decodeOptionsFor(corrected, halfSize) == before);

        negative.lens.lens->origin = LensOrigin::Chosen;
        negative.lens.lens->database = "another";
        CHECK(decodeOptionsFor(photo.with(negative), halfSize) == before);

        DevelopState state = photo.state();
        state.settings.tone.exposure = 1.5F;
        CHECK(decodeOptionsFor(corrected.with(state), halfSize) == before);
    }
    // The demosaic still does, for a file with a mosaic: the rule is about which settings reach
    // the options.
    const Photo mosaiced = openPhoto(test::fixture("bayer-detail-32x24.dng"));
    CHECK_FALSE(decodeOptionsFor(mosaiced.with(NegativeSettings{Demosaic::Vng}), false) ==
                decodeOptionsFor(mosaiced, false));
}

TEST_CASE("The lens round-trips through the JSON object", "[lens][negative][json]") {
    NegativeSettings negative{Demosaic::Dcb};
    // Nothing on and nothing recorded is the text it was before lenses existed.
    REQUIRE(negativeToJson(negative) == R"({"demosaic": "dcb"})");

    negative.lens = {.distortion = true,
                     .vignettingAmount = minimumVignettingAmount,
                     .lateralCa = true,
                     .lens = kitLens()};
    const std::string json = negativeToJson(negative);
    INFO(json);
    REQUIRE_THAT(json, ContainsSubstring(R"("lens": {"distortion": true, "vignetting": 0, )"
                                         R"("lateralCa": true, "recorded": {"maker": "Canon", )"));
    REQUIRE_THAT(json, ContainsSubstring(R"("cropFactor": 1.6, "origin": "matched", )"
                                         R"("database": "2023-11-14T22:13:20Z"}}})"));
    CollectedDiagnostics log;
    REQUIRE(applyNegativeJson(json, NegativeSettings{}, log) == negative);
    REQUIRE(log.entries().empty());

    // Switches without a record.
    negative.lens.lens.reset();
    REQUIRE(applyNegativeJson(negativeToJson(negative), NegativeSettings{}) == negative);
    // A chosen lens keeps its origin.
    negative.lens.lens = kitLens();
    negative.lens.lens->origin = LensOrigin::Chosen;
    REQUIRE(applyNegativeJson(negativeToJson(negative), NegativeSettings{}) == negative);
}

TEST_CASE("An absent lens key leaves the base, and bad ones are warned about",
          "[lens][negative][json]") {
    NegativeSettings base;
    base.lens = {.distortion = true, .lens = kitLens()};
    REQUIRE(applyNegativeJson("{}", base) == base);
    REQUIRE(applyNegativeJson(R"({"lens": {}})", base) == base);

    CollectedDiagnostics log;
    REQUIRE(
        applyNegativeJson(R"({"lens": {"vignetting": true}})", base, log).lens.vignettingAmount ==
        profileVignettingAmount);
    REQUIRE(log.entries().empty());

    // The wrong shapes leave what they would have set, each with a warning.
    for (const char* bad :
         {R"({"lens": true})", R"({"lens": {"distortion": "yes"}})", R"({"lens": {"recorded": 3}})",
          R"({"lens": {"recorded": {"model": "X", "cropFactor": -1}}})",
          R"({"lens": {"recorded": {"model": "X", "origin": "guess"}}})"}) {
        CAPTURE(bad);
        CollectedDiagnostics warned;
        REQUIRE(applyNegativeJson(bad, base, warned) == base);
        REQUIRE(warned.entries().size() == 1);
        REQUIRE(warned.entries().front().notice == Notice::SettingMalformed);
    }
    CollectedDiagnostics unknown;
    REQUIRE(applyNegativeJson(R"({"lens": {"sharpen": 1}})", base, unknown) == base);
    REQUIRE(unknown.entries().size() == 1);
    REQUIRE(unknown.entries().front().notice == Notice::SettingUnknown);

    // A record without a model, for a base with none, is not a record.
    CollectedDiagnostics nameless;
    REQUIRE_FALSE(applyNegativeJson(R"({"lens": {"recorded": {"maker": "Canon"}}})",
                                    NegativeSettings{}, nameless)
                      .lens.lens);
    REQUIRE(nameless.entries().size() == 1);
    // Fields onto a base's record change the record, and keep the rest of it.
    REQUIRE(applyNegativeJson(R"({"lens": {"recorded": {"origin": "chosen"}}})", base)
                .lens.lens->model == base.lens.lens->model);

    // null clears the record, and the switches stay.
    const NegativeSettings cleared = applyNegativeJson(R"({"lens": {"recorded": null}})", base);
    REQUIRE_FALSE(cleared.lens.lens);
    REQUIRE(cleared.lens.distortion);
}

TEST_CASE("The sidecar holds the switches and the lens's name, and nothing of its calibration",
          "[lens][negative][sidecar]") {
    const test::TempDir directory;
    const Photo opened = lensPhotoIn(directory);
    const auto sidecar = sidecarPath(opened.path());

    // A default negative leaves no trace.
    writeSidecar(opened);
    REQUIRE_THAT(textOf(sidecar), !ContainsSubstring("arraw:lens"));

    NegativeSettings negative;
    negative.lens = {.distortion = true,
                     .vignettingAmount = minimumVignettingAmount,
                     .lateralCa = true,
                     .lens = kitLens()};
    const Photo photo = opened.with(negative);
    writeSidecar(photo);

    const std::string text = textOf(sidecar);
    REQUIRE_THAT(text, ContainsSubstring(R"(arraw:lensDistortion="True")"));
    REQUIRE_THAT(text, ContainsSubstring(R"(arraw:lensLateralCa="True")"));
    REQUIRE_THAT(text, !ContainsSubstring("arraw:lensVignetting"));
    REQUIRE_THAT(text, ContainsSubstring(R"(arraw:lensMaker="Canon")"));
    REQUIRE_THAT(text,
                 ContainsSubstring(R"(arraw:lensModel="Canon EF-S 18-135mm f/3.5-5.6 IS USM")"));
    REQUIRE_THAT(text, ContainsSubstring(R"(arraw:lensCropFactor="1.6")"));
    REQUIRE_THAT(text, ContainsSubstring(R"(arraw:lensOrigin="matched")"));
    REQUIRE_THAT(text, ContainsSubstring(R"(arraw:lensDatabase="2023-11-14T22:13:20Z")"));
    // The lens's name, not its numbers: no key for the lens beyond these.
    std::size_t keys = 0;
    for (std::size_t at = text.find("arraw:lens"); at != std::string::npos;
         at = text.find("arraw:lens", at + 1)) {
        ++keys;
    }
    REQUIRE(keys == 5 + 2);

    CollectedDiagnostics log;
    const auto read = readSidecar(opened.path(), log);
    REQUIRE(read);
    REQUIRE(read->negative);
    REQUIRE(read->negative->lens == negative.lens);
    REQUIRE(log.entries().empty());
    REQUIRE(openPhoto(opened.path()).negative() == negative);

    // Switching off keeps the record; clearing the record removes its keys.
    NegativeSettings off = negative;
    off.lens.distortion = false;
    off.lens.lateralCa = false;
    writeSidecar(opened.with(off));
    REQUIRE_THAT(textOf(sidecar), !ContainsSubstring("lensDistortion"));
    REQUIRE_THAT(textOf(sidecar), ContainsSubstring("lensModel"));
    REQUIRE(openPhoto(opened.path()).negative() == off);

    writeSidecar(opened.with(NegativeSettings{}));
    REQUIRE_THAT(textOf(sidecar), !ContainsSubstring("arraw:lens"));
    REQUIRE(openPhoto(opened.path()).negative() == NegativeSettings{});
}

TEST_CASE("A sidecar with only lens keys records a negative and no state",
          "[lens][negative][sidecar]") {
    const test::TempDir directory;
    const Photo opened = lensPhotoIn(directory);
    // A short hand-written sidecar: the lens key and no develop settings at all.
    std::ofstream(sidecarPath(opened.path()))
        << R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
           R"(<rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/1.0/" arraw:lensVignetting="True"/>)"
           R"(</rdf:RDF></x:xmpmeta>)";

    const auto read = readSidecar(opened.path());
    REQUIRE(read);
    REQUIRE_FALSE(read->state);
    REQUIRE(read->negative);
    REQUIRE(read->negative->lens.vignettingAmount == profileVignettingAmount);
    REQUIRE_FALSE(read->negative->lens.lens);
}

TEST_CASE("An absent lens key means off, and what cannot be read is reported and read as off",
          "[lens][negative][sidecar]") {
    const test::TempDir directory;
    const Photo opened = lensPhotoIn(directory);
    NegativeSettings negative;
    negative.lens = {.distortion = true, .lens = kitLens()};
    writeSidecar(opened.with(negative));
    const auto sidecar = sidecarPath(opened.path());

    SECTION("a switch that is neither True nor False") {
        replaceInFile(sidecar, R"(arraw:lensDistortion="True")", R"(arraw:lensDistortion="maybe")");
        CollectedDiagnostics log;
        const auto read = readSidecar(opened.path(), log);
        REQUIRE(read->negative);
        REQUIRE_FALSE(read->negative->lens.distortion);
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
    }
    SECTION("an origin that is not a token reads as matched") {
        replaceInFile(sidecar, R"(arraw:lensOrigin="matched")", R"(arraw:lensOrigin="curated")");
        CollectedDiagnostics log;
        const auto read = readSidecar(opened.path(), log);
        REQUIRE(read->negative->lens.lens);
        REQUIRE(read->negative->lens.lens->origin == LensOrigin::Matched);
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);

        // The file keeps what it cannot read: a write that changes nothing leaves it alone.
        writeSidecar(openPhoto(opened.path()));
        REQUIRE_THAT(textOf(sidecar), ContainsSubstring(R"(arraw:lensOrigin="curated")"));
        // And a write that changes the record replaces it.
        NegativeSettings chosen = openPhoto(opened.path()).negative();
        chosen.lens.lens->origin = LensOrigin::Chosen;
        writeSidecar(opened.with(chosen));
        REQUIRE_THAT(textOf(sidecar), ContainsSubstring(R"(arraw:lensOrigin="chosen")"));
    }
    SECTION("a record with no model or no positive crop factor is no record") {
        replaceInFile(sidecar, R"(arraw:lensCropFactor="1.6")", R"(arraw:lensCropFactor="-2")");
        CollectedDiagnostics log;
        const auto read = readSidecar(opened.path(), log);
        REQUIRE_FALSE(read->negative->lens.lens);
        REQUIRE(read->negative->lens.distortion);
        REQUIRE(log.entries().size() == 1);
    }
    SECTION("a record with no crop factor is no record, and says so") {
        replaceInFile(sidecar, R"( arraw:lensCropFactor="1.6")", "");
        CollectedDiagnostics log;
        const auto read = readSidecar(opened.path(), log);
        REQUIRE_FALSE(read->negative->lens.lens);
        REQUIRE(read->negative->lens.distortion);
        REQUIRE(log.entries().size() == 1);
    }
    SECTION("a switch that is not there is off") {
        replaceInFile(sidecar, R"( arraw:lensDistortion="True")", "");
        const auto read = readSidecar(opened.path());
        REQUIRE_FALSE(read->negative->lens.distortion);
        REQUIRE(read->negative->lens.lens);
    }
}

TEST_CASE("The keys of the lens are known to the sidecar", "[lens][negative][sidecar]") {
    const test::TempDir directory;
    const Photo opened = lensPhotoIn(directory);
    NegativeSettings negative;
    negative.lens = {.distortion = true,
                     .vignettingAmount = profileVignettingAmount,
                     .lateralCa = true,
                     .lens = kitLens()};
    writeSidecar(opened.with(negative));
    CollectedDiagnostics log;
    static_cast<void>(readSidecar(opened.path(), log));
    REQUIRE(log.entries().empty());
}

TEST_CASE("A correction is recorded once, kept when switched off, and not for a non-RAW",
          "[lens][negative][record]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const Photo photo = openPhoto(test::fixture(lensFixture));

    // Nothing to record while nothing is on.
    REQUIRE(recordLensMatch(photo, database) == photo);

    const Photo on = withSwitches(photo, true);
    const Photo recorded = recordLensMatch(on, database);
    REQUIRE(recorded.negative().lens.lens);
    const RecordedLens& lens = *recorded.negative().lens.lens;
    CHECK(lens.maker == "Canon");
    CHECK(lens.model == "Canon EF-S 18-135mm f/3.5-5.6 IS USM");
    CHECK(lens.cropFactor == 1.6);
    CHECK(lens.origin == LensOrigin::Matched);
    CHECK(lens.database == database.stamp());
    // The record is the lens's name; the switches are untouched.
    CHECK(recorded.negative().lens.distortion);
    CHECK(recorded.negative().lens.vignettingAmount == 0.0F);
    CHECK(recorded.negative().demosaic == photo.negative().demosaic);

    // Once: a second call, or one over another database, changes nothing.
    CHECK(recordLensMatch(recorded, database) == recorded);
    const test::TempDir other;
    test::writeTinyDatabase(other.path(), "Other", 1700000000);
    CHECK(recordLensMatch(recorded, LensDatabase::fromDirectory(other.path())) == recorded);

    // Switching off keeps it, and switching on again does not match again.
    const Photo off = withSwitches(recorded, false);
    CHECK(off.negative().lens.lens == recorded.negative().lens.lens);
    CHECK(recordLensMatch(withSwitches(off, false, true), database) ==
          withSwitches(off, false, true));

    // A lens that is not found records nothing; the render says why.
    const test::TempDir tiny;
    test::writeTinyDatabase(tiny.path(), "Nothing like it", 1700000000);
    CHECK(recordLensMatch(on, LensDatabase::fromDirectory(tiny.path())) == on);

    // A photograph that is not a RAW has no profile to find.
    const Photo png = openPhoto(test::fixture("testcard-61x41-srgb8.png"));
    CHECK(recordLensMatch(withSwitches(png, true), database) == withSwitches(png, true));
}

TEST_CASE("Undo takes the toggle and the record away together", "[lens][negative][history]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const Photo photo = openPhoto(test::fixture(lensFixture));
    EditSession session(photo);

    // What the window does: the toggle and the match it records are one step.
    const Photo toggled =
        recordLensMatch(withSwitches(session.photo(), true), test::fixtureDatabase());
    session.setNegative(toggled.negative());
    REQUIRE(session.history().size() == 2);
    REQUIRE(session.photo().negative().lens.lens);

    // Switching off is its own step and keeps the record.
    session.setNegative(withSwitches(session.photo(), false).negative());
    REQUIRE(session.history().size() == 3);
    REQUIRE(session.photo().negative().lens.lens);

    session.undo();
    REQUIRE(session.photo().negative().lens.distortion);
    session.undo();
    REQUIRE(session.photo().negative() == photo.negative());
    REQUIRE_FALSE(session.photo().negative().lens.lens);
    session.redo();
    REQUIRE(session.photo().negative().lens.lens);
}

TEST_CASE("A negative built before an open edit committed keeps the record that commit made",
          "[lens][negative][history]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const Photo photo = openPhoto(test::fixture(lensFixture));
    EditSession session(photo, &test::fixtureDatabase());
    const NegativeSettings allOff = session.photo().negative();

    // A drag of the amount, then a negative read before the drag with every correction off.
    session.begin();
    NegativeSettings dragged = allOff;
    dragged.lens.vignettingAmount = 60.0F;
    session.updateNegative(dragged);
    session.setNegative(allOff);

    REQUIRE_FALSE(session.editing());
    REQUIRE(session.history().size() == 3);
    CHECK(session.history()[1].negative.lens.vignettingAmount == 60.0F);
    REQUIRE(session.history()[1].negative.lens.lens);
    CHECK_FALSE(session.photo().negative().lens.any());
    CHECK(session.photo().negative().lens.lens == session.history()[1].negative.lens.lens);
}

TEST_CASE("A refused negative leaves the session and its open edit as they were",
          "[lens][negative][history]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    EditSession session(openPhoto(test::fixture(lensFixture)), &test::fixtureDatabase());
    session.begin();
    NegativeSettings dragged = session.photo().negative();
    dragged.lens.vignettingAmount = 60.0F;
    session.updateNegative(dragged);
    const Photo before = session.photo();
    const std::size_t steps = session.history().size();

    // Matching itself cannot be made to throw (it fails only on an allocation), so this checks
    // the neighbouring refusal: an invalid negative changes nothing, the open edit included.
    NegativeSettings invalid = dragged;
    invalid.lens.vignettingAmount = 200.0F;
    CHECK_THROWS_AS(session.setNegative(invalid), std::invalid_argument);
    CHECK(session.editing());
    CHECK(session.history().size() == steps);
    CHECK(session.photo() == before);
}

TEST_CASE("A history step names the lens keys that changed", "[lens][negative][history]") {
    const Photo photo = openPhoto(test::fixture(lensFixture));
    const HistoryStep before{photo.state(), EditOrigin::Opened, {}, photo.negative()};

    NegativeSettings negative = photo.negative();
    negative.lens.distortion = true;
    HistoryStep after{photo.state(), EditOrigin::Edit, {}, negative};
    CHECK(describeChange(before, after).negative ==
          std::vector<std::string_view>{"lensDistortion"});

    negative.lens.vignettingAmount = profileVignettingAmount;
    negative.lens.lateralCa = true;
    negative.lens.lens = kitLens();
    after.negative = negative;
    CHECK(
        describeChange(before, after).negative ==
        std::vector<std::string_view>{"lensDistortion", "lensVignetting", "lensLateralCa", "lens"});
    CHECK(describeChange(before, before).negative.empty());

    // A demosaic still comes first.
    negative.demosaic = Demosaic::Vng;
    after.negative = negative;
    CHECK(describeChange(before, after).negative.front() == "demosaic");
}

TEST_CASE("Lens is a copy section of the switches, never of the recorded lens",
          "[lens][negative][copy]") {
    REQUIRE(copySectionNames[static_cast<std::size_t>(CopySection::Lens)] == "lens");
    CHECK(std::ranges::find(copyableSections, CopySection::Lens) != copyableSections.end());
    CHECK(std::ranges::find(defaultCopySections, CopySection::Lens) != defaultCopySections.end());
    CHECK(sectionApplies(CopySection::Lens, true));
    CHECK_FALSE(sectionApplies(CopySection::Lens, false));

    const Photo source = openPhoto(test::fixture(lensFixture));
    NegativeSettings from = source.negative();
    from.lens = {.distortion = true,
                 .vignettingAmount = profileVignettingAmount,
                 .lateralCa = false,
                 .lens = kitLens()};
    const Look look = lookOf(source.with(from));
    CHECK(look.negative.lens == from.lens);

    Photo target = openPhoto(test::fixture("exif-32x24.dng"));
    NegativeSettings own = target.negative();
    own.lens.lateralCa = true;
    RecordedLens ownLens = kitLens();
    ownLens.model = "Canon EF 50mm f/1.8 STM";
    own.lens.lens = ownLens;
    target = target.with(own);

    const std::vector<CopySection> sections{CopySection::Lens};
    const AppliedLook applied = withLook(target, look, sections);
    CHECK(applied.skipped.empty());
    CHECK(applied.negative.lens.distortion);
    CHECK(applied.negative.lens.vignettingAmount == profileVignettingAmount);
    // The look says the third is off, and the section replaces all three.
    CHECK_FALSE(applied.negative.lens.lateralCa);
    // The record is per shot: the target keeps its own.
    CHECK(applied.negative.lens.lens == ownLens);
    CHECK(applied.negative.demosaic == target.negative().demosaic);
    // Nothing else is touched.
    CHECK(applied.state == target.state());

    // Without the section nothing crosses.
    const std::vector<CopySection> exposureOnly{CopySection::Exposure};
    CHECK(withLook(target, look, exposureOnly).negative == target.negative());

    // The state-only overload cannot carry it, as for the demosaic.
    CHECK_THROWS_AS(withLook(target.metadata(), target.state(), look, sections),
                    std::invalid_argument);
}

TEST_CASE("A paste records the target's own match, not the source's", "[lens][negative][copy]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const Photo source = openPhoto(test::fixture("exif-32x24.dng"));
    NegativeSettings from = source.negative();
    from.lens = {.distortion = true, .lens = kitLens()};
    from.lens.lens->model = "Canon EF 50mm f/1.8 STM";
    const Look look = lookOf(source.with(from));

    // The target is the Canon fixture: its match is the kit lens, not the source's record.
    const Photo target = openPhoto(test::fixture(lensFixture));
    const std::vector<CopySection> sections{CopySection::Lens};
    const AppliedLook applied = withLook(target, look, sections);
    REQUIRE(applied.negative.lens.distortion);
    REQUIRE_FALSE(applied.negative.lens.lens);

    const Photo pasted = recordLensMatch(target.with(applied.negative), database);
    REQUIRE(pasted.negative().lens.lens);
    CHECK(pasted.negative().lens.lens->model == "Canon EF-S 18-135mm f/3.5-5.6 IS USM");
    CHECK(pasted.negative().lens.lens->origin == LensOrigin::Matched);
}

TEST_CASE("Lens is skipped from a RAW to another kind, and the other way",
          "[lens][negative][copy]") {
    const Photo raw = openPhoto(test::fixture(lensFixture));
    const Photo png = openPhoto(test::fixture("testcard-61x41-srgb8.png"));
    NegativeSettings on = raw.negative();
    on.lens.distortion = true;
    const std::vector<CopySection> sections{CopySection::Lens};

    const AppliedLook toPng = withLook(png, lookOf(raw.with(on)), sections);
    CHECK(toPng.skipped == std::vector<CopySection>{CopySection::Lens});
    CHECK(toPng.negative == png.negative());

    const AppliedLook toRaw = withLook(raw, lookOf(png), sections);
    CHECK(toRaw.skipped == std::vector<CopySection>{CopySection::Lens});

    // Between two photographs that are not RAWs it applies to neither, and is not reported.
    const AppliedLook between = withLook(png, lookOf(png), sections);
    CHECK(between.skipped.empty());
    CHECK(between.negative == png.negative());
}

TEST_CASE("The vignetting amount is validated", "[lens][negative]") {
    NegativeSettings negative;
    for (const float good : {0.0F, 37.5F, 150.0F}) {
        negative.lens.vignettingAmount = good;
        CHECK_NOTHROW(validate(negative));
    }
    for (const float bad : {-1.0F, 150.5F, std::numeric_limits<float>::quiet_NaN()}) {
        negative.lens.vignettingAmount = bad;
        CHECK_THROWS_AS(validate(negative), std::invalid_argument);
    }
}

TEST_CASE("A vignetting amount above 0 asks for vignetting", "[lens][negative]") {
    for (const auto& [amount, wanted] :
         {std::pair{0.0F, false}, std::pair{0.5F, true}, std::pair{100.0F, true}}) {
        LensSettings settings;
        settings.vignettingAmount = amount;
        CHECK(settings.any() == wanted);
        const LensKinds kinds = kindsOf(settings);
        CHECK(kinds.vignetting == wanted);
        CHECK_FALSE(kinds.distortion);
        CHECK_FALSE(kinds.lateralCa);
    }
}

TEST_CASE("withCorrectionsOf copies the corrections and keeps the lens", "[lens][negative]") {
    const LensSettings from{
        .distortion = true, .vignettingAmount = 60.0F, .lateralCa = true, .lens = std::nullopt};
    LensSettings own;
    own.lens = kitLens();
    const LensSettings result = withCorrectionsOf(own, from);
    CHECK(result.distortion);
    CHECK(result.vignettingAmount == 60.0F);
    CHECK(result.lateralCa);
    CHECK(result.lens == kitLens());
}

TEST_CASE("A paste of the lens section carries the vignetting amount", "[lens][negative][copy]") {
    const Photo source = openPhoto(test::fixture(lensFixture));
    NegativeSettings from = source.negative();
    from.lens.vignettingAmount = 60.0F;
    const Look look = lookOf(source.with(from));
    const Photo target = openPhoto(test::fixture("exif-32x24.dng"));
    const std::vector<CopySection> sections{CopySection::Lens};
    CHECK(withLook(target, look, sections).negative.lens.vignettingAmount == 60.0F);
}

TEST_CASE("A history step names the vignetting key when the amount moves",
          "[lens][negative][history]") {
    const Photo photo = openPhoto(test::fixture(lensFixture));
    NegativeSettings negative = photo.negative();
    negative.lens.vignettingAmount = profileVignettingAmount;
    const HistoryStep before{photo.state(), EditOrigin::Edit, {}, negative};
    negative.lens.vignettingAmount = 60.0F;
    const HistoryStep after{photo.state(), EditOrigin::Edit, {}, negative};
    CHECK(describeChange(before, after).negative ==
          std::vector<std::string_view>{"lensVignetting"});
}

TEST_CASE("The vignetting amount round-trips through the JSON object and the sidecar",
          "[lens][negative][json][sidecar]") {
    const test::TempDir directory;
    const Photo opened = lensPhotoIn(directory);
    const auto sidecar = sidecarPath(opened.path());
    for (const float amount : {0.0F, 37.5F, 100.0F, 150.0F}) {
        CAPTURE(amount);
        NegativeSettings negative;
        negative.lens = {.distortion = true, .vignettingAmount = amount, .lens = kitLens()};

        const std::string json = negativeToJson(negative);
        CollectedDiagnostics log;
        REQUIRE(applyNegativeJson(json, NegativeSettings{}, log) == negative);
        REQUIRE(log.entries().empty());

        writeSidecar(opened.with(negative));
        if (amount == 0.0F) {
            REQUIRE_THAT(textOf(sidecar), !ContainsSubstring("lensVignetting"));
        } else {
            REQUIRE_THAT(textOf(sidecar),
                         ContainsSubstring("arraw:lensVignetting=\"" +
                                           std::string(amount == 37.5F    ? "37.5"
                                                       : amount == 100.0F ? "100"
                                                                          : "150") +
                                           "\""));
        }
        CollectedDiagnostics sidecarLog;
        const auto read = readSidecar(opened.path(), sidecarLog);
        REQUIRE(read);
        REQUIRE(read->negative);
        REQUIRE(read->negative->lens == negative.lens);
        REQUIRE(sidecarLog.entries().empty());
        std::filesystem::remove(sidecar);
    }
}

TEST_CASE("The legacy vignetting switch reads as 100 or 0", "[lens][negative][json][sidecar]") {
    for (const auto& [json, expected] :
         {std::pair{R"({"lens": {"vignetting": true}})", profileVignettingAmount},
          std::pair{R"({"lens": {"vignetting": false}})", minimumVignettingAmount}}) {
        CAPTURE(json);
        NegativeSettings base;
        base.lens.vignettingAmount = 42.0F;
        CollectedDiagnostics log;
        REQUIRE(applyNegativeJson(json, base, log).lens.vignettingAmount == expected);
        REQUIRE(log.entries().empty());
    }

    const test::TempDir directory;
    const Photo opened = lensPhotoIn(directory);
    const auto sidecar = sidecarPath(opened.path());
    NegativeSettings negative;
    negative.lens = {.distortion = true, .vignettingAmount = 100.0F, .lens = kitLens()};
    writeSidecar(opened.with(negative));
    const std::string written = textOf(sidecar);
    REQUIRE_THAT(written, ContainsSubstring(R"(arraw:lensVignetting="100")"));

    for (const auto& [spelling, expected] : {std::pair{"True", 100.0F}, std::pair{"true", 100.0F},
                                             std::pair{"False", 0.0F}, std::pair{"false", 0.0F}}) {
        CAPTURE(spelling);
        replaceInFile(sidecar, R"(arraw:lensVignetting="100")",
                      std::string("arraw:lensVignetting=\"") + spelling + "\"");
        CollectedDiagnostics log;
        const auto read = readSidecar(opened.path(), log);
        REQUIRE(read->negative);
        REQUIRE(read->negative->lens.vignettingAmount == expected);
        REQUIRE(log.entries().empty());
        std::ofstream(sidecar) << written;
    }

    // An untouched legacy True survives a save that changes something else.
    replaceInFile(sidecar, R"(arraw:lensVignetting="100")", R"(arraw:lensVignetting="True")");
    NegativeSettings changed = openPhoto(opened.path()).negative();
    REQUIRE(changed.lens.vignettingAmount == profileVignettingAmount);
    changed.lens.lateralCa = true;
    writeSidecar(opened.with(changed));
    REQUIRE_THAT(textOf(sidecar), ContainsSubstring(R"(arraw:lensVignetting="True")"));
    REQUIRE_THAT(textOf(sidecar), ContainsSubstring(R"(arraw:lensLateralCa="True")"));
}

TEST_CASE("A vignetting amount out of range is clamped and logged",
          "[lens][negative][json][sidecar]") {
    for (const auto& [text, expected, read] : {std::tuple{"200", maximumVignettingAmount, 200.0},
                                               std::tuple{"-5", minimumVignettingAmount, -5.0}}) {
        CAPTURE(text);
        CollectedDiagnostics log;
        const NegativeSettings fromJson = applyNegativeJson(
            std::string(R"({"lens": {"vignetting": )") + text + "}}", NegativeSettings{}, log);
        REQUIRE(fromJson.lens.vignettingAmount == expected);
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingClamped);
        REQUIRE(std::get<std::string>(log.entries().front().values.at(0)) == "lens.vignetting");
        REQUIRE(std::get<double>(log.entries().front().values.at(1)) == read);
        REQUIRE(std::get<double>(log.entries().front().values.at(2)) ==
                static_cast<double>(expected));

        const test::TempDir directory;
        const Photo opened = lensPhotoIn(directory);
        std::ofstream(sidecarPath(opened.path()))
            << R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
               R"(<rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/1.0/" arraw:lensVignetting=")"
            << text << R"("/></rdf:RDF></x:xmpmeta>)";
        CollectedDiagnostics sidecarLog;
        const auto sidecar = readSidecar(opened.path(), sidecarLog);
        REQUIRE(sidecar->negative->lens.vignettingAmount == expected);
        REQUIRE(sidecarLog.entries().size() == 1);
        REQUIRE(sidecarLog.entries().front().notice == Notice::SettingClamped);
        REQUIRE(std::get<std::string>(sidecarLog.entries().front().values.at(0)) ==
                "lensVignetting");
    }
}

TEST_CASE("A vignetting amount that is no number is reported and the base is kept",
          "[lens][negative][json][sidecar]") {
    NegativeSettings base;
    base.lens = {.vignettingAmount = 60.0F, .lens = kitLens()};
    for (const char* bad :
         {R"({"lens": {"vignetting": "abc"}})", R"({"lens": {"vignetting": [1]}})"}) {
        CAPTURE(bad);
        CollectedDiagnostics log;
        REQUIRE(applyNegativeJson(bad, base, log) == base);
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
    }

    const test::TempDir directory;
    const Photo opened = lensPhotoIn(directory);
    const auto sidecar = sidecarPath(opened.path());
    for (const char* bad : {"abc", "nan", "[1]"}) {
        CAPTURE(bad);
        std::ofstream(sidecar)
            << R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
               R"(<rdf:Description rdf:about="" xmlns:arraw="http://ns.arraw.org/develop/1.0/" arraw:lensVignetting=")"
            << bad << R"("/></rdf:RDF></x:xmpmeta>)";
        CollectedDiagnostics log;
        const auto read = readSidecar(opened.path(), log);
        REQUIRE(read->negative);
        REQUIRE(read->negative->lens.vignettingAmount == minimumVignettingAmount);
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
    }
}

TEST_CASE("Amount 0 writes no vignetting key, and no lens settings write the old JSON",
          "[lens][negative][json][sidecar]") {
    CHECK(negativeToJson(NegativeSettings{}) == R"({"demosaic": "ahd"})");
    NegativeSettings negative;
    negative.lens = {.lateralCa = true};
    CHECK_THAT(negativeToJson(negative),
               ContainsSubstring(R"("vignetting": 0, "lateralCa": true)"));

    const test::TempDir directory;
    const Photo opened = lensPhotoIn(directory);
    writeSidecar(opened.with(negative));
    REQUIRE_THAT(textOf(sidecarPath(opened.path())), !ContainsSubstring("lensVignetting"));
}

namespace {

/// @brief Gives the lens fixture's session with the fixture database attached.
EditSession recordingSession(const Photo& photo) {
    return EditSession(photo, &test::fixtureDatabase());
}

} // namespace

TEST_CASE("The session holds the lens database it is given, without owning it",
          "[lens][negative][record][session]") {
    const Photo photo = openPhoto(test::fixture(lensFixture));
    EditSession session(photo);
    CHECK(session.lensDatabase() == nullptr);
    session.setLensDatabase(&test::fixtureDatabase());
    CHECK(session.lensDatabase() == &test::fixtureDatabase());
    CHECK(EditSession(photo, &test::fixtureDatabase()).lensDatabase() == &test::fixtureDatabase());
    const EditSession copy = session;
    CHECK(copy.lensDatabase() == &test::fixtureDatabase());
    session.setLensDatabase(nullptr);
    CHECK(session.lensDatabase() == nullptr);
}

TEST_CASE("A step that turns a correction on records the match in the same step",
          "[lens][negative][record][session]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const Photo photo = openPhoto(test::fixture(lensFixture));
    EditSession session = recordingSession(photo);

    session.setNegative(withSwitches(photo, true).negative());
    REQUIRE(session.history().size() == 2);
    REQUIRE(session.photo().negative().lens.lens);
    CHECK(session.photo().negative().lens.lens->origin == LensOrigin::Matched);
    CHECK(session.history().back().negative == session.photo().negative());

    // Undo takes the toggle and the record away together, and redo brings both back.
    session.undo();
    CHECK(session.photo().negative() == photo.negative());
    session.redo();
    CHECK(session.photo().negative().lens.lens);
    CHECK(session.hasUnsavedChanges());

    SECTION("a drag records at its commit, and a drag that ends at 0 records nothing") {
        EditSession drag = recordingSession(photo);
        drag.begin();
        NegativeSettings negative = drag.photo().negative();
        negative.lens.vignettingAmount = 60.0F;
        drag.updateNegative(negative);
        CHECK_FALSE(drag.photo().negative().lens.lens);
        negative.lens.vignettingAmount = 80.0F;
        drag.updateNegative(negative);
        CHECK_FALSE(drag.photo().negative().lens.lens);
        drag.commit();
        REQUIRE(drag.history().size() == 2);
        CHECK(drag.photo().negative().lens.lens);
        CHECK(drag.photo().negative().lens.vignettingAmount == 80.0F);

        EditSession back = recordingSession(photo);
        back.begin();
        negative.lens.vignettingAmount = 60.0F;
        back.updateNegative(negative);
        negative.lens.vignettingAmount = 0.0F;
        back.updateNegative(negative);
        back.commit();
        CHECK(back.history().size() == 1);
        CHECK_FALSE(back.photo().negative().lens.lens);
    }

    SECTION("cancel, and a develop-only step, record nothing") {
        EditSession cancelled = recordingSession(photo);
        cancelled.begin();
        cancelled.updateNegative(withSwitches(photo, true).negative());
        cancelled.cancel();
        CHECK(cancelled.photo() == photo);

        // A correction already on, with no lens recorded: an Exposure step leaves it unrecorded.
        EditSession unrecorded(withSwitches(photo, true));
        unrecorded.setLensDatabase(&test::fixtureDatabase());
        DevelopState state = unrecorded.photo().state();
        state.settings.tone.exposure = 0.5F;
        unrecorded.setState(state);
        REQUIRE(unrecorded.history().size() == 2);
        CHECK(unrecorded.photo().negative().lens.any());
        CHECK_FALSE(unrecorded.photo().negative().lens.lens);

        // Nor does a demosaic-only step.
        NegativeSettings negative = unrecorded.photo().negative();
        negative.demosaic =
            negative.demosaic == Demosaic::Linear ? Demosaic::Ahd : Demosaic::Linear;
        unrecorded.setNegative(negative);
        CHECK_FALSE(unrecorded.photo().negative().lens.lens);

        // Navigation does not record either.
        unrecorded.undo();
        unrecorded.redo();
        unrecorded.goTo(0);
        CHECK_FALSE(unrecorded.photo().negative().lens.lens);
    }
}

TEST_CASE("The session never replaces a recorded lens, and a reset matches the same lens again",
          "[lens][negative][record][session]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const Photo photo = openPhoto(test::fixture(lensFixture));
    EditSession session = recordingSession(photo);
    session.setNegative(withSwitches(photo, true).negative());
    const NegativeSettings matched = session.photo().negative();
    REQUIRE(matched.lens.lens);

    // Clearing the record gives the same match with the same stamp: no step.
    NegativeSettings cleared = matched;
    cleared.lens.lens.reset();
    session.setNegative(cleared);
    CHECK(session.history().size() == 2);
    CHECK(session.photo().negative() == matched);

    // A chosen lens stays, whatever the next step does.
    NegativeSettings chosen = matched;
    chosen.lens.lens->origin = LensOrigin::Chosen;
    chosen.lens.lens->model = "Another lens";
    session.setNegative(chosen);
    REQUIRE(session.history().size() == 3);
    NegativeSettings more = chosen;
    more.lens.lateralCa = true;
    session.setNegative(more);
    CHECK(session.photo().negative().lens.lens == chosen.lens.lens);
}

TEST_CASE("A paste records the target's match through the two-part step",
          "[lens][negative][record][session]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const Photo photo = openPhoto(test::fixture(lensFixture));
    EditSession session = recordingSession(photo);
    const Photo source = withSwitches(photo, true, true, true);

    session.setState(source.negative(), session.photo().state(), EditOrigin::Paste);
    REQUIRE(session.history().size() == 2);
    REQUIRE(session.photo().negative().lens.lens);
    CHECK(session.photo().negative().lens.lens->origin == LensOrigin::Matched);

    // An open edit is committed first, and its own change of lens settings records.
    EditSession open = recordingSession(photo);
    open.begin();
    open.updateNegative(withSwitches(photo, true).negative());
    open.setState(open.photo().negative(), open.photo().state());
    CHECK(open.history().size() == 2);
    CHECK(open.photo().negative().lens.lens);
}

TEST_CASE("With no database the step is made as given, and one that arrives records nothing back",
          "[lens][negative][record][session]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const Photo photo = openPhoto(test::fixture(lensFixture));
    EditSession session(photo);
    session.setNegative(withSwitches(photo, true).negative());
    REQUIRE(session.history().size() == 2);
    CHECK(session.photo().negative().lens.any());
    CHECK_FALSE(session.photo().negative().lens.lens);

    // Arrival touches neither the photograph nor the history.
    const Photo before = session.photo();
    session.setLensDatabase(&test::fixtureDatabase());
    CHECK(session.photo() == before);
    CHECK(session.history().size() == 2);

    // The next step that changes the lens settings records it.
    NegativeSettings next = session.photo().negative();
    next.lens.lateralCa = true;
    session.setNegative(next);
    CHECK(session.photo().negative().lens.lens);

    // An edit open when it arrives records at its commit.
    EditSession open(photo);
    open.begin();
    open.updateNegative(withSwitches(photo, true).negative());
    open.setLensDatabase(&test::fixtureDatabase());
    CHECK_FALSE(open.photo().negative().lens.lens);
    open.commit();
    CHECK(open.photo().negative().lens.lens);
}

TEST_CASE("Saving with an open edit writes the photograph the commit leaves",
          "[lens][negative][record][session][sidecar]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const test::TempDir directory;
    const Photo photo = lensPhotoIn(directory);
    EditSession session = recordingSession(photo);

    session.begin();
    session.updateNegative(withSwitches(photo, true).negative());
    REQUIRE_FALSE(session.photo().negative().lens.lens);
    session.save();

    CHECK_FALSE(session.editing());
    CHECK(session.photo().negative().lens.lens);
    CHECK_FALSE(session.hasUnsavedChanges());
    CHECK(session.saved() == session.photo());
    CHECK(session.history().size() == 2);
    CHECK(session.history().back().negative == session.photo().negative());
    const Photo reopened = openPhoto(photo.path());
    CHECK(reopened.negative() == session.photo().negative());
}
