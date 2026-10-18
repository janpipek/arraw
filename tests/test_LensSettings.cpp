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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
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
    negative.lens.vignetting = vignetting;
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
    CHECK_FALSE(settings.vignetting);
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
        negative.lens = {
            .distortion = true, .vignetting = true, .lateralCa = true, .lens = kitLens()};
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

    negative.lens = {.distortion = true, .vignetting = false, .lateralCa = true, .lens = kitLens()};
    const std::string json = negativeToJson(negative);
    INFO(json);
    REQUIRE_THAT(json, ContainsSubstring(R"("lens": {"distortion": true, "vignetting": false, )"
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
    REQUIRE(applyNegativeJson(R"({"lens": {"vignetting": true}})", base, log).lens.vignetting);
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
    negative.lens = {.distortion = true, .vignetting = false, .lateralCa = true, .lens = kitLens()};
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
    REQUIRE(read->negative->lens.vignetting);
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
    negative.lens = {.distortion = true, .vignetting = true, .lateralCa = true, .lens = kitLens()};
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
    CHECK_FALSE(recorded.negative().lens.vignetting);
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

TEST_CASE("A history step names the lens keys that changed", "[lens][negative][history]") {
    const Photo photo = openPhoto(test::fixture(lensFixture));
    const HistoryStep before{photo.state(), EditOrigin::Opened, {}, photo.negative()};

    NegativeSettings negative = photo.negative();
    negative.lens.distortion = true;
    HistoryStep after{photo.state(), EditOrigin::Edit, {}, negative};
    CHECK(describeChange(before, after).negative ==
          std::vector<std::string_view>{"lensDistortion"});

    negative.lens.vignetting = true;
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
    from.lens = {.distortion = true, .vignetting = true, .lateralCa = false, .lens = kitLens()};
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
    CHECK(applied.negative.lens.vignetting);
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
