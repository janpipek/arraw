#include "WhiteBalanceChoice.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace arraw;
using namespace arraw::app;

/// The pure rules behind the White Balance panel's combo box and its Temp and
/// Tint rows, tested without widgets.

namespace {

ColorSettings custom(std::optional<float> kelvin, std::optional<float> tint) {
    return {WhiteBalanceMode::Custom, kelvin, tint};
}

} // namespace

TEST_CASE("The presets are Lightroom's lights", "[WhiteBalanceChoice]") {
    CHECK(whiteBalancePresets().size() == 6);
    CHECK(lightOf(WhiteBalanceChoice::Daylight) == ColourTemperature{5500.0F, 10.0F});
    CHECK(lightOf(WhiteBalanceChoice::Cloudy) == ColourTemperature{6500.0F, 10.0F});
    CHECK(lightOf(WhiteBalanceChoice::Shade) == ColourTemperature{7500.0F, 10.0F});
    CHECK(lightOf(WhiteBalanceChoice::Tungsten) == ColourTemperature{2850.0F, 0.0F});
    CHECK(lightOf(WhiteBalanceChoice::Fluorescent) == ColourTemperature{3800.0F, 21.0F});
    CHECK(lightOf(WhiteBalanceChoice::Flash) == ColourTemperature{5500.0F, 0.0F});
    CHECK(whiteBalanceChoices().front() == WhiteBalanceChoice::AsShot);
    CHECK(whiteBalanceChoices().back() == WhiteBalanceChoice::Custom);
}

TEST_CASE("As Shot and Custom name no light", "[WhiteBalanceChoice]") {
    CHECK(!lightOf(WhiteBalanceChoice::AsShot).has_value());
    CHECK(!lightOf(WhiteBalanceChoice::Custom).has_value());
}

TEST_CASE("The combo describes the settings", "[WhiteBalanceChoice]") {
    SECTION("As Shot in that mode, whatever values linger") {
        CHECK(choiceOf({}) == WhiteBalanceChoice::AsShot);
        CHECK(choiceOf({WhiteBalanceMode::AsShot, 5500.0F, 10.0F}) == WhiteBalanceChoice::AsShot);
    }
    SECTION("a preset when both values match it") {
        CHECK(choiceOf(custom(2850.0F, 0.0F)) == WhiteBalanceChoice::Tungsten);
        CHECK(choiceOf(custom(7500.0F, 10.0F)) == WhiteBalanceChoice::Shade);
    }
    SECTION("Daylight wins over Flash, which shares its temperature") {
        CHECK(choiceOf(custom(5500.0F, 10.0F)) == WhiteBalanceChoice::Daylight);
        CHECK(choiceOf(custom(5500.0F, 0.0F)) == WhiteBalanceChoice::Flash);
    }
    SECTION("Custom otherwise") {
        CHECK(choiceOf(custom(5500.0F, 11.0F)) == WhiteBalanceChoice::Custom);
        CHECK(choiceOf(custom(5500.0F, std::nullopt)) == WhiteBalanceChoice::Custom);
        CHECK(choiceOf(custom(std::nullopt, 10.0F)) == WhiteBalanceChoice::Custom);
        CHECK(choiceOf(custom(std::nullopt, std::nullopt)) == WhiteBalanceChoice::Custom);
    }
}

TEST_CASE("Every preset reads back as itself", "[WhiteBalanceChoice]") {
    for (const WhiteBalancePreset& preset : whiteBalancePresets()) {
        CAPTURE(preset.name);
        const ColourTemperature light = preset.light;
        const WhiteBalanceChoice back = choiceOf(custom(light.kelvin, light.tint));
        // Flash shares Daylight's temperature but not its tint, so every one is unique.
        CHECK(back == preset.choice);
        CHECK(!nameOf(preset.choice).isEmpty());
    }
}

TEST_CASE("The rows show the camera's reading for whatever is absent", "[WhiteBalanceChoice]") {
    const ColourTemperature camera{4321.0F, -6.0F};
    CHECK(shownLight({}, camera) == camera);
    CHECK(shownLight({WhiteBalanceMode::AsShot, 9000.0F, 50.0F}, camera) == camera);
    CHECK(shownLight(custom(3000.0F, std::nullopt), camera) == ColourTemperature{3000.0F, -6.0F});
    CHECK(shownLight(custom(std::nullopt, 8.0F), camera) == ColourTemperature{4321.0F, 8.0F});
    CHECK(shownLight(custom(3000.0F, 8.0F), camera) == ColourTemperature{3000.0F, 8.0F});
}
