#include "SettingCodec.h"
#include "support/Sentinels.h"

#include <SettingDescriptors.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace arraw;

/// The codec (SettingCodec.h): one neutral value per leaf type, which the
/// document formats map to their own syntax.

namespace {

const FieldDescriptor& row(std::string_view key) {
    const FieldDescriptor* descriptor = findDescriptor(key);
    REQUIRE(descriptor != nullptr);
    return *descriptor;
}

constexpr double notANumber = std::numeric_limits<double>::quiet_NaN();
constexpr double unbounded = std::numeric_limits<double>::infinity();

/// Decodes into defaults and returns them, leaving the diagnostics in @p log.
DevelopSettings decoded(std::string_view key, const Encoded& encoded, CollectedDiagnostics& log) {
    DevelopSettings settings;
    decode(row(key), encoded, settings, log);
    return settings;
}

} // namespace

TEST_CASE("Every row decodes what it encoded", "[settings][codec]") {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        DevelopSettings original;
        test::setNonDefault(descriptor, original);
        REQUIRE_FALSE(original == DevelopSettings{});

        CollectedDiagnostics log;
        DevelopSettings restored;
        decode(descriptor, encode(descriptor, original), restored, log);

        REQUIRE(restored == original);
        REQUIRE(log.entries().empty());
    }
}

TEST_CASE("Values encode to the shapes the format table names", "[settings][codec]") {
    DevelopSettings settings;
    settings.tone.exposure = 0.1F;
    settings.color.whiteBalance = WhiteBalanceMode::Custom;
    settings.geometry.rotation = QuarterTurn::Clockwise180;
    settings.geometry.flipVertical = true;
    settings.geometry.crop.rectangle =
        UprightCropRect{.left = 0.25, .top = 0.0, .right = 1.0, .bottom = 0.5};
    settings.geometry.crop.aspect = CropRatio{1.5};

    // A float is spelled as short as it can be, not as its double expansion.
    REQUIRE(std::get<double>(encode(row("exposure"), settings)) == 0.1);
    REQUIRE(std::holds_alternative<std::monostate>(encode(row("temperature"), settings)));
    REQUIRE(std::get<std::string>(encode(row("whiteBalance"), settings)) == "custom");
    REQUIRE(std::get<std::string>(encode(row("rotation"), settings)) == "clockwise180");
    REQUIRE(std::get<bool>(encode(row("flipVertical"), settings)));
    REQUIRE(std::get<Compound>(encode(row("cropRectangle"), settings)) ==
            Compound{{"left", 0.25}, {"top", 0.0}, {"right", 1.0}, {"bottom", 0.5}});
    REQUIRE(std::get<Compound>(encode(row("cropAspect"), settings)) == Compound{{"ratio", 1.5}});

    settings.geometry.crop.aspect = FreeCropAspect{};
    REQUIRE(std::get<std::string>(encode(row("cropAspect"), settings)) == "free");
    settings.geometry.crop.aspect = OriginalCropAspect{};
    REQUIRE(std::get<std::string>(encode(row("cropAspect"), settings)) == "original");
}

TEST_CASE("A number out of range is clamped with a warning", "[settings][codec]") {
    const FieldDescriptor& exposure = row("exposure");
    CollectedDiagnostics log;

    REQUIRE(decoded("exposure", 1000.0, log).tone.exposure == brightestExposure);
    REQUIRE(decoded("exposure", -1000.0, log).tone.exposure == darkestExposure);
    REQUIRE(decoded("temperature", 50.0, log).color.temperature == warmestKelvin);
    REQUIRE(decoded("straighten", 90.0, log).geometry.straighten == maximumStraighten);

    REQUIRE(log.entries().size() == 4);
    const Diagnostic& first = log.entries().front();
    REQUIRE(first.notice == Notice::SettingClamped);
    REQUIRE(first.severity == Severity::Warning);
    REQUIRE(first.values.size() == 3);
    REQUIRE(std::get<std::string>(first.values[0]) == exposure.key);
    REQUIRE(std::get<double>(first.values[1]) == 1000.0);
    REQUIRE(std::get<double>(first.values[2]) == exposure.range->maximum);
}

TEST_CASE("A number within range is taken as it is", "[settings][codec]") {
    CollectedDiagnostics log;
    REQUIRE(decoded("straighten", 45.0, log).geometry.straighten == 45.0);
    REQUIRE(decoded("tint", -150.0, log).color.tint == -tintLimit);
    REQUIRE(log.entries().empty());
}

TEST_CASE("A value of the wrong shape is skipped with a warning", "[settings][codec]") {
    const Compound someMap{{"x", 1.0}};
    const struct {
        std::string_view key;
        Encoded value;
    } cases[] = {
        {"exposure", std::string("bright")},
        {"exposure", true},
        {"exposure", Encoded{}},
        {"exposure", someMap},
        {"straighten", std::string("1")},
        {"temperature", std::string("warm")},
        {"flipHorizontal", 1.0},
        {"flipVertical", std::string("true")},
        {"whiteBalance", 1.0},
        {"whiteBalance", std::string("daylight")},
        {"whiteBalance", Encoded{}},
        {"rotation", std::string("clockwise45")},
        {"rotation", std::string("Clockwise90")},
        {"cropRectangle", 1.0},
        {"cropRectangle", someMap},
        {"cropAspect", 1.5},
        {"cropAspect", std::string("square")},
        {"cropAspect", Encoded{}},
    };
    for (const auto& [key, value] : cases) {
        INFO(key);
        CollectedDiagnostics log;
        REQUIRE(decoded(key, value, log) == DevelopSettings{});
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
        REQUIRE(log.entries().front().severity == Severity::Warning);
        REQUIRE(std::get<std::string>(log.entries().front().values.at(0)) == key);
    }
}

TEST_CASE("A skipped value leaves the field as it was", "[settings][codec]") {
    DevelopSettings settings;
    settings.tone.exposure = 1.5F;
    settings.geometry.crop.aspect = CropRatio{2.0};
    CollectedDiagnostics log;
    decode(row("exposure"), std::string("nope"), settings, log);
    decode(row("cropAspect"), Compound{{"ratio", -1.0}}, settings, log);
    REQUIRE(settings.tone.exposure == 1.5F);
    REQUIRE(settings.geometry.crop.aspect == CropAspect{CropRatio{2.0}});
    REQUIRE(log.entries().size() == 2);
}

TEST_CASE("A number that is not finite is malformed, not clamped", "[settings][codec]") {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (!descriptor.range) {
            continue;
        }
        for (const double bad : {notANumber, unbounded, -unbounded}) {
            INFO(descriptor.key << " " << bad);
            CollectedDiagnostics log;
            DevelopSettings settings;
            decode(descriptor, bad, settings, log);
            REQUIRE(settings == DevelopSettings{});
            REQUIRE(log.entries().size() == 1);
            REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
        }
    }

    CollectedDiagnostics log;
    REQUIRE(decoded("cropRectangle",
                    Compound{{"left", notANumber}, {"top", 0.0}, {"right", 1.0}, {"bottom", 1.0}},
                    log) == DevelopSettings{});
    REQUIRE(log.entries().size() == 1);
}

TEST_CASE("Null unsets an optional and nothing else", "[settings][codec]") {
    DevelopSettings settings;
    settings.color.temperature = 5000.0F;
    settings.color.tint = 10.0F;
    settings.geometry.crop.rectangle = UprightCropRect{};
    CollectedDiagnostics log;
    for (const char* key : {"temperature", "tint", "cropRectangle"}) {
        decode(row(key), Encoded{}, settings, log);
    }
    REQUIRE(log.entries().empty());
    REQUIRE(settings == DevelopSettings{});
}

TEST_CASE("A crop ratio must be positive and finite", "[settings][codec]") {
    for (const double bad : {0.0, -1.5, notANumber, unbounded}) {
        INFO(bad);
        CollectedDiagnostics log;
        REQUIRE(decoded("cropAspect", Compound{{"ratio", bad}}, log) == DevelopSettings{});
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
    }
    CollectedDiagnostics log;
    REQUIRE(decoded("cropAspect", Compound{{"ratio", 0.5}}, log).geometry.crop.aspect ==
            CropAspect{CropRatio{0.5}});
    REQUIRE(log.entries().empty());
}

TEST_CASE("A crop rectangle needs exactly its four edges", "[settings][codec]") {
    CollectedDiagnostics log;
    const Compound five{
        {"left", 0.0}, {"top", 0.0}, {"right", 1.0}, {"bottom", 1.0}, {"extra", 0.0}};
    const Compound renamed{{"left", 0.0}, {"top", 0.0}, {"right", 1.0}, {"below", 1.0}};
    REQUIRE(decoded("cropRectangle", five, log) == DevelopSettings{});
    REQUIRE(decoded("cropRectangle", renamed, log) == DevelopSettings{});
    REQUIRE(log.entries().size() == 2);
}

TEST_CASE("The subject is carried onto the warning", "[settings][codec]") {
    CollectedDiagnostics log;
    DevelopSettings settings;
    decode(row("exposure"), 1000.0, settings, log, std::filesystem::path("IMG_1.xmp"));
    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().subject == std::filesystem::path("IMG_1.xmp"));
}

TEST_CASE("The new notices describe themselves", "[settings][codec][diagnostics]") {
    REQUIRE(describe({.notice = Notice::SettingClamped,
                      .severity = Severity::Warning,
                      .values = {std::string("exposure"), 1000.0, 5.0}}) ==
            "'exposure' is 1000, outside what it accepts, so 5 was used");
    REQUIRE(describe({.notice = Notice::SettingUnknown,
                      .severity = Severity::Warning,
                      .values = {std::string("sparkle")}}) ==
            "'sparkle' is not a setting, so it was ignored");
    REQUIRE(describe({.notice = Notice::SettingMalformed,
                      .severity = Severity::Warning,
                      .values = {std::string("exposure"), std::string("a number")}}) ==
            "'exposure' cannot be read, so it was ignored; expected a number");
    REQUIRE(describe({.notice = Notice::NewerSettingsVersion,
                      .severity = Severity::Warning,
                      .values = {2.0, 1.0}}) ==
            "these settings are version 2, but this arraw knows up to version 1; "
            "reading what it can");
}

TEST_CASE("Floats that a plain cast would move by a unit still round trip", "[settings][codec]") {
    // Casting the double back rounds twice; these two land one ulp off.
    for (const std::uint32_t bits : {0x15ae43fdU, 0x95ae43fdU}) {
        INFO(bits);
        DevelopSettings original;
        original.tone.exposure = std::bit_cast<float>(bits);
        DevelopSettings restored;
        CollectedDiagnostics log;
        decode(row("exposure"), encode(row("exposure"), original), restored, log);
        REQUIRE(std::bit_cast<std::uint32_t>(restored.tone.exposure) == bits);
        REQUIRE(log.entries().empty());
    }
}

TEST_CASE("A crop rectangle the geometry plan would refuse is skipped", "[settings][codec]") {
    const Compound inverted{{"left", 0.75}, {"top", 0.0}, {"right", 0.25}, {"bottom", 1.0}};
    const Compound empty{{"left", 0.5}, {"top", 0.0}, {"right", 0.5}, {"bottom", 1.0}};
    const Compound outside{{"left", -0.1}, {"top", 0.0}, {"right", 1.0}, {"bottom", 1.0}};
    const Compound beyond{{"left", 0.0}, {"top", 0.0}, {"right", 1.0}, {"bottom", 1.5}};
    for (const Compound& bad : {inverted, empty, outside, beyond}) {
        CollectedDiagnostics log;
        REQUIRE(decoded("cropRectangle", bad, log) == DevelopSettings{});
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
    }
    // Members may come in any order.
    CollectedDiagnostics log;
    const Compound shuffled{{"bottom", 0.5}, {"right", 1.0}, {"top", 0.0}, {"left", 0.25}};
    REQUIRE(decoded("cropRectangle", shuffled, log).geometry.crop.rectangle ==
            UprightCropRect{.left = 0.25, .top = 0.0, .right = 1.0, .bottom = 0.5});
    REQUIRE(log.entries().empty());
}

TEST_CASE("Every row says what it expects", "[settings][codec]") {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        REQUIRE_FALSE(expectation(descriptor).empty());
    }
    REQUIRE(expectation(row("rotation")) == "one of none, clockwise90, clockwise180, clockwise270");
    REQUIRE(expectation(row("whiteBalance")) == "one of asShot, custom");

    CollectedDiagnostics log;
    reportMalformed(row("exposure"), log, std::filesystem::path("IMG_1.xmp"));
    REQUIRE(log.entries().size() == 1);
    REQUIRE(log.entries().front().subject == std::filesystem::path("IMG_1.xmp"));
    REQUIRE(describe(log.entries().front()) ==
            "'exposure' cannot be read, so it was ignored; expected a number");
}

TEST_CASE("A clamp reports its numbers in full", "[settings][codec][diagnostics]") {
    REQUIRE(describe({.notice = Notice::SettingClamped,
                      .severity = Severity::Warning,
                      .values = {std::string("exposure"), 5.0000001, 5.0}}) ==
            "'exposure' is 5.0000001, outside what it accepts, so 5 was used");
    REQUIRE(describe({.notice = Notice::SettingClamped,
                      .severity = Severity::Warning,
                      .values = {std::string("exposure"), 1234567.0, 5.0}}) ==
            "'exposure' is 1234567, outside what it accepts, so 5 was used");
}

TEST_CASE("A tone curve encodes to its points and decodes them back exactly", "[settings][codec]") {
    DevelopSettings settings;
    settings.toneCurve.red.points = {{0.0F, 0.1F}, {0.3F, 0.35F}, {1.0F, 0.9F}};
    const Encoded encoded = encode(row("toneCurveRed"), settings);
    // Floats spell as short as they can, like every other float.
    REQUIRE(std::get<PointList>(encoded) == PointList{{0.0, 0.1}, {0.3, 0.35}, {1.0, 0.9}});
    REQUIRE(std::get<PointList>(encode(row("toneCurveLuma"), settings)) ==
            PointList{{0.0, 0.0}, {1.0, 1.0}});
    REQUIRE(takesPoints(row("toneCurveBlue")));
    REQUIRE_FALSE(takesPoints(row("exposure")));
    REQUIRE_FALSE(takesPoints(row("cropRectangle")));
    REQUIRE(compoundShapes(row("toneCurveLuma")).empty());

    CollectedDiagnostics log;
    REQUIRE(decoded("toneCurveRed", encoded, log) == settings);
    REQUIRE(log.entries().empty());
}

TEST_CASE("A point list is read from x,y pairs joined by semicolons", "[settings][codec]") {
    REQUIRE(parsePointList("0,0;0.25,0.2;1,1") == PointList{{0.0, 0.0}, {0.25, 0.2}, {1.0, 1.0}});
    // Blanks are ignored, a leading plus is taken, and the order is kept.
    REQUIRE(parsePointList(" 1 , +1 ;\t0,0 ") == PointList{{1.0, 1.0}, {0.0, 0.0}});
    // Values are not judged here: that is each caller's policy.
    REQUIRE(parsePointList("-5,7") == PointList{{-5.0, 7.0}});
    for (const char* bad : {"", ";", "0,0;", ";0,0", "0", "0,0,0", "0;1", "a,b", "0,0;1,", "0 0",
                            "0,0;;1,1", "++1,1", "1,1x"}) {
        INFO(bad);
        REQUIRE_FALSE(parsePointList(bad));
    }
}

TEST_CASE("A tone curve's points may come in any order", "[settings][codec]") {
    CollectedDiagnostics log;
    const DevelopSettings result =
        decoded("toneCurveLuma", PointList{{1.0, 1.0}, {0.5, 0.25}, {0.0, 0.0}}, log);
    const std::vector<CurvePoint> expected{{0.0F, 0.0F}, {0.5F, 0.25F}, {1.0F, 1.0F}};
    REQUIRE(result.toneCurve.luma.points == expected);
    REQUIRE(log.entries().empty());
}

TEST_CASE("A tone curve's end x within rounding of 0 or 1 is taken as exactly there",
          "[settings][codec]") {
    CollectedDiagnostics log;
    const DevelopSettings result =
        decoded("toneCurveLuma", PointList{{1.0000004, 1.0}, {0.99, 0.5}, {-3.0e-7, 0.0}}, log);
    const std::vector<CurvePoint> expected{{0.0F, 0.0F}, {0.99F, 0.5F}, {1.0F, 1.0F}};
    REQUIRE(result.toneCurve.luma.points == expected);
    REQUIRE(log.entries().empty());
}

TEST_CASE("A tone curve's y out of range is clamped with a warning", "[settings][codec]") {
    CollectedDiagnostics log;
    const DevelopSettings result =
        decoded("toneCurveGreen", PointList{{0.0, -0.5}, {0.5, 0.5}, {1.0, 1.5}}, log);
    const std::vector<CurvePoint> expected{{0.0F, 0.0F}, {0.5F, 0.5F}, {1.0F, 1.0F}};
    REQUIRE(result.toneCurve.green.points == expected);
    REQUIRE(log.entries().size() == 2);
    const Diagnostic& first = log.entries().front();
    REQUIRE(first.notice == Notice::SettingClamped);
    REQUIRE(std::get<std::string>(first.values[0]) == "toneCurveGreen");
    REQUIRE(std::get<double>(first.values[1]) == -0.5);
    REQUIRE(std::get<double>(first.values[2]) == 0.0);
}

TEST_CASE("A tone curve that cannot be one is skipped with a warning", "[settings][codec]") {
    PointList seventeen;
    for (int i = 0; i < 17; ++i) {
        seventeen.emplace_back(i / 16.0, 0.5);
    }
    const struct {
        const char* what;
        Encoded value;
    } cases[] = {
        {"a number", 1.0},
        {"a string", std::string("0,0;1,1")},
        {"a compound", Compound{{"x", 1.0}}},
        {"null", Encoded{}},
        {"no points", PointList{}},
        {"one point", PointList{{0.0, 0.0}}},
        {"too many points", seventeen},
        {"a repeated x", PointList{{0.0, 0.0}, {0.5, 0.2}, {0.5, 0.3}, {1.0, 1.0}}},
        {"a start after zero", PointList{{0.25, 0.0}, {1.0, 1.0}}},
        {"an end before one", PointList{{0.0, 0.0}, {0.75, 1.0}}},
        {"a coordinate not a number", PointList{{0.0, 0.0}, {0.5, notANumber}, {1.0, 1.0}}},
        {"a coordinate not finite", PointList{{0.0, 0.0}, {unbounded, 0.5}, {1.0, 1.0}}},
        // An x is never clamped: moving a point along x makes it another curve.
        {"a first x just below zero", PointList{{-0.01, 0.0}, {0.5, 0.5}, {1.0, 1.0}}},
        {"a last x just above one", PointList{{0.0, 0.0}, {0.5, 0.5}, {1.001, 1.0}}},
        {"an x far out", PointList{{0.0, 0.0}, {1.0, 0.5}, {2.0, 1.0}}},
        {"an x too far out for a float", PointList{{0.0, 0.0}, {1.0, 0.5}, {1.0e300, 1.0}}},
        {"points closer than a hundredth",
         PointList{{0.0, 0.0}, {0.5, 0.2}, {0.505, 0.3}, {1.0, 1.0}}},
    };
    for (const auto& [what, value] : cases) {
        INFO(what);
        CollectedDiagnostics log;
        REQUIRE(decoded("toneCurveBlue", value, log) == DevelopSettings{});
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::SettingMalformed);
    }
    // A skipped curve keeps what the field held.
    DevelopSettings settings;
    settings.toneCurve.red.points = {{0.0F, 0.2F}, {1.0F, 1.0F}};
    const DevelopSettings before = settings;
    CollectedDiagnostics log;
    decode(row("toneCurveRed"), PointList{{0.0, 0.0}}, settings, log);
    REQUIRE(settings == before);
}
