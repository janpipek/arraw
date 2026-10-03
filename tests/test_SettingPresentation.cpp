#include "SettingPresentation.h"

#include <SettingDescriptors.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace arraw;
using namespace arraw::app;
using Catch::Approx;

/// What the develop panel derives from the descriptor table (ADR 008).

namespace {

/// Settings that no panel shows yet; deciding where a new one goes is deliberate.
constexpr std::array<std::string_view, 10> notShownYet{
    "rotation",   "flipHorizontal", "flipVertical", "straighten",     "cropRectangle",
    "cropAspect", "toneCurveLuma",  "toneCurveRed", "toneCurveGreen", "toneCurveBlue"};

/// Settings the Treatment buttons edit; they have no slider row.
constexpr std::array<std::string_view, 1> shownByTreatment{"convertToGrayscale"};

/// Every key that has a slider row, across all groups of the panel.
std::vector<std::string_view> slidingKeys() {
    std::vector<std::string_view> keys(toneKeys().begin(), toneKeys().end());
    keys.insert(keys.end(), whiteBalanceKeys().begin(), whiteBalanceKeys().end());
    keys.insert(keys.end(), colorKeys().begin(), colorKeys().end());
    for (int page = 0; page < hslPageCount; ++page) {
        keys.insert(keys.end(), hslKeys(page).begin(), hslKeys(page).end());
    }
    keys.insert(keys.end(), blackAndWhiteKeys().begin(), blackAndWhiteKeys().end());
    return keys;
}

/// Settings the combo box of the White Balance group edits; they have no row of their own.
constexpr std::array<std::string_view, 1> shownByCombo{"whiteBalance"};

bool isNumber(const FieldDescriptor& descriptor) {
    return std::visit(
        [](auto accessor) {
            using Field = std::remove_cvref_t<decltype(accessor(std::declval<DevelopSettings&>()))>;
            return std::is_same_v<Field, float> || std::is_same_v<Field, double> ||
                   std::is_same_v<Field, std::optional<float>>;
        },
        descriptor.member);
}

} // namespace

TEST_CASE("Every shown key has a descriptor, a range, a numeric leaf and a presentation",
          "[SettingPresentation]") {
    for (const std::string_view key : slidingKeys()) {
        CAPTURE(key);
        const FieldDescriptor* descriptor = findDescriptor(key);
        REQUIRE(descriptor != nullptr);
        CHECK(descriptor->range.has_value());
        CHECK(isNumber(*descriptor));
        const SettingPresentation& presentation = presentationOf(key);
        CHECK(!presentation.label.isEmpty());
        CHECK(!presentation.toolTip.isEmpty());
        CHECK(presentation.step > 0.0);
        CHECK(presentation.decimals >= 0);
    }
}

TEST_CASE("Every setting is either shown or listed as not shown yet", "[SettingPresentation]") {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        CAPTURE(descriptor.key);
        const std::vector<std::string_view> sliding = slidingKeys();
        const bool shown =
            std::ranges::find(sliding, descriptor.key) != sliding.end() ||
            std::ranges::find(shownByCombo, descriptor.key) != shownByCombo.end() ||
            std::ranges::find(shownByTreatment, descriptor.key) != shownByTreatment.end();
        const bool listed = std::ranges::find(notShownYet, descriptor.key) != notShownYet.end();
        CHECK(shown != listed);
    }
    for (const std::string_view key : notShownYet) {
        CAPTURE(key);
        CHECK(findDescriptor(key) != nullptr);
    }
}

TEST_CASE("An unknown key has no presentation", "[SettingPresentation]") {
    CHECK_THROWS_AS(presentationOf("rotation"), std::out_of_range);
}

TEST_CASE("Slider ticks map to values and back", "[SettingPresentation]") {
    const SettingRange range = *findDescriptor("exposure")->range;
    constexpr double step = 0.01;

    CHECK(tickCount(range, step) == 1000);
    CHECK(tickOf(0.0, range, step) == 500);
    for (const double value : {-5.0, -1.25, 0.0, 0.37, 5.0}) {
        CAPTURE(value);
        CHECK(valueOfTick(tickOf(value, range, step), range, step) == Approx(value).margin(1e-9));
    }
    CHECK(tickOf(-9.0, range, step) == 0);
    CHECK(tickOf(9.0, range, step) == 1000);
    CHECK(valueOfTick(-3, range, step) == Approx(-5.0));
    CHECK(valueOfTick(5000, range, step) == Approx(5.0));
}

TEST_CASE("Defaults come from a default-constructed DevelopSettings", "[SettingPresentation]") {
    CHECK(defaultValueOf(*findDescriptor("exposure")) == 0.0);
    CHECK(defaultValueOf(*findDescriptor("filmicHighlights")) == 25.0);
    CHECK_THROWS_AS(defaultValueOf(*findDescriptor("rotation")), std::invalid_argument);
}

TEST_CASE("An optional number counts as a numeric row", "[SettingPresentation]") {
    for (const std::string_view key : whiteBalanceKeys()) {
        CAPTURE(key);
        const FieldDescriptor& descriptor = *findDescriptor(key);
        CHECK(descriptor.range.has_value());
        CHECK(descriptor.applies == Applicability::RawOnly);
    }
}

TEST_CASE("The temperature slider is even in mired", "[SettingPresentation]") {
    const SettingRange range = *findDescriptor("temperature")->range;
    const SettingPresentation& presentation = presentationOf("temperature");
    constexpr SliderScale scale = SliderScale::Reciprocal;
    REQUIRE(presentation.scale == scale);
    const double step = presentation.step;
    const int count = tickCount(range, step, scale);

    SECTION("the ends are the range's ends") {
        CHECK(count == reciprocalTickCount);
        CHECK(tickOf(range.minimum, range, step, scale) == 0);
        CHECK(tickOf(range.maximum, range, step, scale) == count);
        CHECK(valueOfTick(0, range, step, scale) == Approx(2000.0));
        CHECK(valueOfTick(count, range, step, scale) == Approx(12000.0));
        CHECK(tickOf(100.0, range, step, scale) == 0);
        CHECK(tickOf(90000.0, range, step, scale) == count);
        CHECK(valueOfTick(-5, range, step, scale) == Approx(2000.0));
        CHECK(valueOfTick(count + 5, range, step, scale) == Approx(12000.0));
    }
    SECTION("values rise with the tick, in whole steps") {
        double previous = 0.0;
        for (int tick = 0; tick <= count; ++tick) {
            const double value = valueOfTick(tick, range, step, scale);
            CAPTURE(tick, value);
            CHECK(value >= previous);
            CHECK(std::fmod(value, step) == Approx(0.0).margin(1e-9));
            previous = value;
        }
    }
    SECTION("the middle tick is the mired midpoint") {
        // 1e6/2000 = 500 mired, 1e6/12000 = 83.3; the middle is 291.7 mired, 3429 K.
        CHECK(valueOfTick(count / 2, range, step, scale) == Approx(3429.0).margin(step));
        CHECK(tickOf(3429.0, range, step, scale) == Approx(count / 2).margin(2));
    }
    SECTION("a tick maps back to itself or a neighbour of the same tidy value") {
        for (int tick = 0; tick <= count; tick += 7) {
            const double value = valueOfTick(tick, range, step, scale);
            CAPTURE(tick, value);
            // Rounding to the step can fold several warm ticks into one value.
            CHECK(valueOfTick(tickOf(value, range, step, scale), range, step, scale) ==
                  Approx(value).margin(1e-9));
        }
    }
    SECTION("a value maps to a tick near it") {
        for (const double value : {2000.0, 2850.0, 3800.0, 5500.0, 7500.0, 12000.0}) {
            CAPTURE(value);
            const double back = valueOfTick(tickOf(value, range, step, scale), range, step, scale);
            // One tick is 0.42 mired: 1.7 K at 2000 K and 60 K at 12000 K.
            CHECK(back == Approx(value).margin(value * value * 0.5e-6 + step));
        }
    }
}

TEST_CASE("A linear scale given explicitly is the old mapping", "[SettingPresentation]") {
    const SettingRange range = *findDescriptor("exposure")->range;
    for (const double value : {-5.0, -1.25, 0.0, 0.37, 5.0}) {
        CHECK(tickOf(value, range, 0.01, SliderScale::Linear) == tickOf(value, range, 0.01));
    }
    CHECK(tickCount(range, 0.01, SliderScale::Linear) == 1000);
    CHECK(valueOfTick(123, range, 0.01, SliderScale::Linear) == valueOfTick(123, range, 0.01));
}

TEST_CASE("Tint is a plain linear row", "[SettingPresentation]") {
    const SettingPresentation& presentation = presentationOf("tint");
    CHECK(presentation.scale == SliderScale::Linear);
    CHECK(presentation.decimals == 0);
    CHECK(presentation.step == 1.0);
    CHECK(presentation.unit.isEmpty());
    const SettingRange range = *findDescriptor("tint")->range;
    CHECK(tickCount(range, presentation.step) == 300);
    CHECK(tickOf(0.0, range, presentation.step) == 150);
}

TEST_CASE("The band pages list eight distinct keys each", "[SettingPresentation]") {
    std::vector<std::string_view> all;
    for (int page = 0; page < hslPageCount; ++page) {
        CHECK(hslKeys(page).size() == 8);
        all.insert(all.end(), hslKeys(page).begin(), hslKeys(page).end());
    }
    CHECK(blackAndWhiteKeys().size() == 8);
    all.insert(all.end(), blackAndWhiteKeys().begin(), blackAndWhiteKeys().end());
    std::ranges::sort(all);
    CHECK(std::ranges::adjacent_find(all) == all.end());
    CHECK_THROWS_AS(hslKeys(hslPageCount), std::out_of_range);
    CHECK_THROWS_AS(hslKeys(-1), std::out_of_range);
}

TEST_CASE("Band rows are named for their band and move in whole units", "[SettingPresentation]") {
    for (int page = 0; page < hslPageCount; ++page) {
        CHECK(presentationOf(hslKeys(page).front()).label == QString("Reds"));
        CHECK(presentationOf(hslKeys(page).back()).label == QString("Magentas"));
    }
    CHECK(presentationOf("grayAqua").label == QString("Aquas"));
    CHECK(presentationOf("saturation").label == QString("Saturation"));
    CHECK(presentationOf("vibrance").label == QString("Vibrance"));
    for (const std::string_view key : slidingKeys()) {
        if (key != "exposure" && key != "temperature") {
            CAPTURE(key);
            CHECK(presentationOf(key).step == 1.0);
            CHECK(presentationOf(key).decimals == 0);
        }
    }
}

TEST_CASE("Black and white swaps the Color and HSL groups for the mix", "[SettingPresentation]") {
    const TreatmentVisibility colour = visibleGroups(false);
    CHECK(colour.color);
    CHECK(colour.hsl);
    CHECK_FALSE(colour.blackAndWhiteMix);

    const TreatmentVisibility grey = visibleGroups(true);
    CHECK_FALSE(grey.color);
    CHECK_FALSE(grey.hsl);
    CHECK(grey.blackAndWhiteMix);
}
