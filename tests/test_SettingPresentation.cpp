#include "SettingPresentation.h"

#include <SettingDescriptors.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <type_traits>

using namespace arraw;
using namespace arraw::app;
using Catch::Approx;

/// What the develop panel derives from the descriptor table (ADR 008).

namespace {

/// Settings that no panel shows yet; deciding where a new one goes is deliberate.
constexpr std::array<std::string_view, 9> notShownYet{
    "whiteBalance", "temperature", "tint",          "rotation",  "flipHorizontal",
    "flipVertical", "straighten",  "cropRectangle", "cropAspect"};

bool isNumber(const FieldDescriptor& descriptor) {
    return std::visit(
        [](auto accessor) {
            using Field = std::remove_cvref_t<decltype(accessor(std::declval<DevelopSettings&>()))>;
            return std::is_same_v<Field, float> || std::is_same_v<Field, double>;
        },
        descriptor.member);
}

} // namespace

TEST_CASE("Every shown key has a descriptor, a range, a numeric leaf and a presentation",
          "[SettingPresentation]") {
    for (const std::string_view key : toneKeys()) {
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
        const bool shown = std::ranges::find(toneKeys(), descriptor.key) != toneKeys().end();
        const bool listed = std::ranges::find(notShownYet, descriptor.key) != notShownYet.end();
        CHECK(shown != listed);
    }
    for (const std::string_view key : notShownYet) {
        CAPTURE(key);
        CHECK(findDescriptor(key) != nullptr);
    }
}

TEST_CASE("An unknown key has no presentation", "[SettingPresentation]") {
    CHECK_THROWS_AS(presentationOf("temperature"), std::out_of_range);
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
