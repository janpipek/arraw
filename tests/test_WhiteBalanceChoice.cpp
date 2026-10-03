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
    CHECK(withChoice({}, WhiteBalanceChoice::Daylight) == custom(5500.0F, 10.0F));
    CHECK(withChoice({}, WhiteBalanceChoice::Cloudy) == custom(6500.0F, 10.0F));
    CHECK(withChoice({}, WhiteBalanceChoice::Shade) == custom(7500.0F, 10.0F));
    CHECK(withChoice({}, WhiteBalanceChoice::Tungsten) == custom(2850.0F, 0.0F));
    CHECK(withChoice({}, WhiteBalanceChoice::Fluorescent) == custom(3800.0F, 21.0F));
    CHECK(withChoice({}, WhiteBalanceChoice::Flash) == custom(5500.0F, 0.0F));
    CHECK(whiteBalanceChoices().front() == WhiteBalanceChoice::AsShot);
    CHECK(whiteBalanceChoices().back() == WhiteBalanceChoice::Custom);
}

TEST_CASE("Choosing As Shot forgets the values", "[WhiteBalanceChoice]") {
    CHECK(withChoice(custom(4000.0F, 3.0F), WhiteBalanceChoice::AsShot) == ColorSettings{});
}

TEST_CASE("The Custom entry changes nothing", "[WhiteBalanceChoice]") {
    const ColorSettings settings = custom(4000.0F, std::nullopt);
    CHECK(withChoice(settings, WhiteBalanceChoice::Custom) == settings);
    CHECK(withChoice({}, WhiteBalanceChoice::Custom) == ColorSettings{});
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
        const WhiteBalanceChoice back = choiceOf(withChoice({}, preset.choice));
        // Flash shares Daylight's temperature but not its tint, so every one is unique.
        CHECK(back == preset.choice);
        CHECK(!nameOf(preset.choice).isEmpty());
    }
}

TEST_CASE("Moving Temp leaves Tint where it was", "[WhiteBalanceChoice]") {
    SECTION("from As Shot, tint stays the camera's") {
        CHECK(withTemperature({}, 4200.0F) == custom(4200.0F, std::nullopt));
    }
    SECTION("from As Shot with stale values, they are ignored") {
        CHECK(withTemperature({WhiteBalanceMode::AsShot, 3000.0F, 9.0F}, 4200.0F) ==
              custom(4200.0F, std::nullopt));
    }
    SECTION("from Custom, a set tint stays") {
        CHECK(withTemperature(custom(5000.0F, 7.0F), 4200.0F) == custom(4200.0F, 7.0F));
    }
}

TEST_CASE("Moving Tint leaves Temp where it was", "[WhiteBalanceChoice]") {
    CHECK(withTint({}, 12.0F) == custom(std::nullopt, 12.0F));
    CHECK(withTint(custom(5000.0F, std::nullopt), 12.0F) == custom(5000.0F, 12.0F));
}

TEST_CASE("Resetting a row clears its value, and both cleared is As Shot", "[WhiteBalanceChoice]") {
    CHECK(withTemperature(custom(5000.0F, 7.0F), std::nullopt) == custom(std::nullopt, 7.0F));
    CHECK(withTint(custom(5000.0F, 7.0F), std::nullopt) == custom(5000.0F, std::nullopt));
    CHECK(withTemperature(custom(5000.0F, std::nullopt), std::nullopt) == ColorSettings{});
    CHECK(withTint(custom(std::nullopt, 7.0F), std::nullopt) == ColorSettings{});
    CHECK(withTemperature(ColorSettings{}, std::nullopt) == ColorSettings{});
}

TEST_CASE("The rows show the camera's reading for whatever is absent", "[WhiteBalanceChoice]") {
    const ColourTemperature camera{4321.0F, -6.0F};
    CHECK(shownLight({}, camera) == camera);
    CHECK(shownLight({WhiteBalanceMode::AsShot, 9000.0F, 50.0F}, camera) == camera);
    CHECK(shownLight(custom(3000.0F, std::nullopt), camera) == ColourTemperature{3000.0F, -6.0F});
    CHECK(shownLight(custom(std::nullopt, 8.0F), camera) == ColourTemperature{4321.0F, 8.0F});
    CHECK(shownLight(custom(3000.0F, 8.0F), camera) == ColourTemperature{3000.0F, 8.0F});
}
