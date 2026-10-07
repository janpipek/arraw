#include "support/Sentinels.h"

#include <SettingDescriptors.h>
#include <SettingsJson.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;

/// Settings as JSON (SettingsJson.h): every key written, present keys applied.

namespace {

DevelopSettings allNonDefault() {
    DevelopSettings settings;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        test::setNonDefault(descriptor, settings);
    }
    return settings;
}

/// Wraps a body of `"key": value` pairs into a version 1 document.
std::string document(const std::string& entries) {
    return R"({"arraw": 1, "settings": {)" + entries + "}}";
}

} // namespace

TEST_CASE("Every row survives a round trip through JSON", "[settings][json]") {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        DevelopSettings original;
        test::setNonDefault(descriptor, original);
        REQUIRE(applySettingsJson(settingsToJson(original), DevelopSettings{}) == original);
    }
    const DevelopSettings everything = allNonDefault();
    REQUIRE(applySettingsJson(settingsToJson(everything), DevelopSettings{}) == everything);
    REQUIRE(applySettingsJson(settingsToJson(DevelopSettings{}), everything) == DevelopSettings{});
}

TEST_CASE("The document holds every key once, in table order", "[settings][json]") {
    const std::string text = settingsToJson(DevelopSettings{});
    REQUIRE(text.starts_with("{\n  \"arraw\": 1,\n  \"settings\": {\n    \"exposure\": 0,\n"));
    REQUIRE(text.ends_with("\"cropAspect\": \"free\"\n  }\n}\n"));

    std::size_t from = text.find("\"settings\"");
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        const std::string quoted = '"' + std::string(descriptor.key) + '"';
        const std::size_t at = text.find(quoted, from);
        REQUIRE(at != std::string::npos);
        REQUIRE(text.find(quoted, at + 1) == std::string::npos);
        from = at;
    }
}

TEST_CASE("Values are written the way a person would", "[settings][json]") {
    DevelopSettings settings;
    settings.tone.exposure = 0.1F;
    settings.color.temperature = 5500.0F;
    settings.geometry.rotation = QuarterTurn::Clockwise90;
    settings.geometry.flipHorizontal = true;
    settings.geometry.crop.rectangle =
        UprightCropRect{.left = 0.25, .top = 0.0, .right = 1.0, .bottom = 0.5};
    settings.geometry.crop.aspect = CropRatio{1.5};
    const std::string text = settingsToJson(settings);
    for (const char* line : {"\"exposure\": 0.1,", "\"temperature\": 5500,", "\"tint\": null,",
                             "\"rotation\": \"clockwise90\",", "\"flipHorizontal\": true,",
                             "\"whiteBalance\": \"asShot\",",
                             "\"cropRectangle\": {\"left\": 0.25, \"top\": 0, \"right\": 1, "
                             "\"bottom\": 0.5},",
                             "\"cropAspect\": {\"ratio\": 1.5}"}) {
        INFO(line);
        REQUIRE(text.find(line) != std::string::npos);
    }
}

TEST_CASE("Only the keys present are applied", "[settings][json]") {
    DevelopSettings base;
    base.tone.contrast = 0.5F;
    base.geometry.flipVertical = true;

    CollectedDiagnostics log;
    const DevelopSettings result = applySettingsJson(document(R"("exposure": 1.5)"), base, log);

    DevelopSettings expected = base;
    expected.tone.exposure = 1.5F;
    REQUIRE(result == expected);
    REQUIRE(log.entries().empty());
}

TEST_CASE("Null unsets an optional in the base", "[settings][json]") {
    DevelopSettings base;
    base.color.temperature = 4000.0F;
    base.geometry.crop.rectangle = UprightCropRect{};
    const DevelopSettings result =
        applySettingsJson(document(R"("temperature": null, "cropRectangle": null)"), base);
    REQUIRE_FALSE(result.color.temperature);
    REQUIRE_FALSE(result.geometry.crop.rectangle);
}

TEST_CASE("Null for a setting that cannot be unset is malformed", "[settings][json]") {
    CollectedDiagnostics log;
    DevelopSettings base;
    base.tone.exposure = 1.0F;
    REQUIRE(applySettingsJson(document(R"("exposure": null)"), base, log) == base);
    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
}

TEST_CASE("An out-of-range number is clamped and reported", "[settings][json]") {
    CollectedDiagnostics log;
    const DevelopSettings result =
        applySettingsJson(document(R"("exposure": 99, "straighten": -60.5)"), {}, log);
    REQUIRE(result.tone.exposure == brightestExposure);
    REQUIRE(result.geometry.straighten == minimumStraighten);
    REQUIRE(log.entries().size() == 2);
    REQUIRE(log.entries()[0].notice == Notice::SettingClamped);
    REQUIRE(std::get<double>(log.entries()[0].values[1]) == 99.0);
    REQUIRE(log.entries()[1].notice == Notice::SettingClamped);
    REQUIRE(std::get<double>(log.entries()[1].values[1]) == -60.5);
    REQUIRE_NOTHROW(validate(result));
}

TEST_CASE("An unknown key is reported and ignored", "[settings][json]") {
    CollectedDiagnostics log;
    const DevelopSettings result =
        applySettingsJson(document(R"("sparkle": 3, "exposure": 1)"), {}, log);
    REQUIRE(result.tone.exposure == 1.0F);
    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().notice == Notice::SettingUnknown);
    REQUIRE(std::get<std::string>(log.entries().front().values.at(0)) == "sparkle");
}

TEST_CASE("A value of the wrong type is reported and skipped", "[settings][json]") {
    for (const char* entry :
         {R"("exposure": "bright")", R"("exposure": [1])", R"("flipHorizontal": 1)",
          R"("whiteBalance": "daylight")", R"("rotation": 90)", R"("cropAspect": "square")",
          R"("cropAspect": {"ratio": 0})",
          R"("cropRectangle": {"left": "a", "top": 0, "right": 1, "bottom": 1})",
          R"("cropRectangle": [0, 0, 1, 1])"}) {
        INFO(entry);
        CollectedDiagnostics log;
        const DevelopSettings result = applySettingsJson(document(entry), {}, log);
        REQUIRE(result == DevelopSettings{});
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
    }
}

TEST_CASE("A number too large to be finite rejects the whole document", "[settings][json]") {
    // Qt refuses the text (IllegalNumber), so no infinity gets in and nothing is applied.
    REQUIRE_THROWS_AS(applySettingsJson(document(R"("exposure": 1e999)"), {}),
                      std::invalid_argument);
}

TEST_CASE("Reports come in table order, then unknown keys alphabetically", "[settings][json]") {
    CollectedDiagnostics log;
    const DevelopSettings result = applySettingsJson(
        R"({"arraw": 3, "settings": {"zebra": 1, "cropAspect": [1], "apple": 2,
            "exposure": 99, "mango": 3}})",
        {}, log);
    REQUIRE(result.tone.exposure == brightestExposure);
    REQUIRE(log.entries().size() == 6);
    REQUIRE(log.entries()[0].notice == Notice::NewerSettingsVersion);
    REQUIRE(log.entries()[1].notice == Notice::SettingClamped);
    REQUIRE(log.entries()[2].notice == Notice::SettingMalformed);
    REQUIRE(std::get<std::string>(log.entries()[2].values.at(0)) == "cropAspect");
    REQUIRE(std::get<std::string>(log.entries()[2].values.at(1)) ==
            "free, original, or a positive finite ratio");
    for (std::size_t index = 3; index < 6; ++index) {
        REQUIRE(log.entries()[index].notice == Notice::SettingUnknown);
    }
    REQUIRE(std::get<std::string>(log.entries()[3].values.at(0)) == "apple");
    REQUIRE(std::get<std::string>(log.entries()[4].values.at(0)) == "mango");
    REQUIRE(std::get<std::string>(log.entries()[5].values.at(0)) == "zebra");
}

TEST_CASE("A crop rectangle that is not in order is skipped", "[settings][json]") {
    CollectedDiagnostics log;
    const DevelopSettings result = applySettingsJson(
        document(R"("cropRectangle": {"left": 0.8, "top": 0, "right": 0.2, "bottom": 1})"), {},
        log);
    REQUIRE(result == DevelopSettings{});
    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
}

TEST_CASE("A repeated key keeps its last value without a warning", "[settings][json]") {
    CollectedDiagnostics log;
    const DevelopSettings result =
        applySettingsJson(document(R"("exposure": 1, "exposure": 2)"), {}, log);
    REQUIRE(result.tone.exposure == 2.0F);
    REQUIRE(log.entries().empty());
}

TEST_CASE("A newer version is read, with a warning", "[settings][json]") {
    CollectedDiagnostics log;
    const DevelopSettings result = applySettingsJson(
        R"({"arraw": 2, "settings": {"exposure": 1, "futureThing": true}})", {}, log);
    REQUIRE(result.tone.exposure == 1.0F);
    REQUIRE(log.entries().size() == 2);
    REQUIRE(log.entries()[0].notice == Notice::NewerSettingsVersion);
    REQUIRE(std::get<double>(log.entries()[0].values.at(0)) == 2.0);
    REQUIRE(log.entries()[1].notice == Notice::SettingUnknown);

    CollectedDiagnostics current;
    (void)applySettingsJson(document(""), {}, current);
    REQUIRE(current.entries().empty());
}

TEST_CASE("A document that is not a settings document throws", "[settings][json]") {
    for (const char* text :
         {"", "not json", R"({"arraw": 1, "settings": )", "[]", "3", R"({"settings": {}})",
          R"({"arraw": 1})", R"({"arraw": "1", "settings": {}})", R"({"arraw": 0, "settings": {}})",
          R"({"arraw": 1.5, "settings": {}})", R"({"arraw": 1, "settings": []})",
          R"({"arraw": 1, "settings": null})"}) {
        INFO(text);
        REQUIRE_THROWS_AS(applySettingsJson(text, {}), std::invalid_argument);
    }
}

TEST_CASE("Settings that are not valid are not written", "[settings][json]") {
    DevelopSettings settings;
    settings.tone.exposure = 1000.0F;
    REQUIRE_THROWS_AS(settingsToJson(settings), std::invalid_argument);
}

TEST_CASE("A tone curve is written as a list of points", "[settings][json]") {
    DevelopSettings settings;
    settings.toneCurve.red.points = {{0.0F, 0.1F}, {0.25F, 0.5F}, {1.0F, 1.0F}};
    const std::string text = settingsToJson(settings);
    REQUIRE(text.find("\"toneCurveLuma\": [[0, 0], [1, 1]],") != std::string::npos);
    REQUIRE(text.find("\"toneCurveRed\": [[0, 0.1], [0.25, 0.5], [1, 1]],") != std::string::npos);

    CollectedDiagnostics log;
    REQUIRE(applySettingsJson(text, {}, log) == settings);
    REQUIRE(log.entries().empty());
}

TEST_CASE("A tone curve is read from a list of pairs", "[settings][json]") {
    CollectedDiagnostics log;
    const DevelopSettings result = applySettingsJson(
        document(
            R"("toneCurveGreen": [[1, 1], [0.5, 0.4], [0, 0]], "toneCurveBlue": [[0, 0.5], [1, 3]])"),
        {}, log);
    const std::vector<CurvePoint> green{{0.0F, 0.0F}, {0.5F, 0.4F}, {1.0F, 1.0F}};
    const std::vector<CurvePoint> blue{{0.0F, 0.5F}, {1.0F, 1.0F}};
    REQUIRE(result.toneCurve.green.points == green);
    REQUIRE(result.toneCurve.blue.points == blue);
    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().notice == Notice::SettingClamped);
}

TEST_CASE("A tone curve that is not a list of valid pairs is reported and skipped",
          "[settings][json]") {
    for (const char* entry :
         {R"("toneCurveLuma": 1)", R"("toneCurveLuma": [])", R"("toneCurveLuma": [[0, 0]])",
          R"("toneCurveLuma": [[0, 0, 0], [1, 1]])", R"("toneCurveLuma": [[0, "a"], [1, 1]])",
          R"("toneCurveLuma": [0, 1])",
          R"("toneCurveLuma": [[0, 0], [0.5, 0.5], [0.5, 0.6], [1, 1]])",
          R"("toneCurveLuma": {"x": 1})", R"("toneCurveLuma": null)"}) {
        INFO(entry);
        CollectedDiagnostics log;
        const DevelopSettings result = applySettingsJson(document(entry), {}, log);
        REQUIRE(result == DevelopSettings{});
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
    }
}
