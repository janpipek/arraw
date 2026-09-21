#include "WhiteBalance.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace arraw;

namespace {

/// @brief CIE XYZ (D65) to linear Rec.2020.
///
/// Published reference data, deliberately not shared with the engine: a test
/// that used the same constant as the code could not catch a wrong one.
constexpr Matrix3 xyzToRec2020{{1.7166512F, -0.3556708F, -0.2533663F, //
                                -0.6666844F, 1.6164812F, 0.0157685F,  //
                                0.0176399F, -0.0427706F, 0.9421031F}};

/// @brief Builds a camera that records a given light as neutral.
///
/// Its channels are the working space's own primaries, so the only thing under
/// test is the temperature maths rather than a change of primaries as well.
/// @param x Chromaticity of the light.
/// @param y Chromaticity of the light.
/// @return A sensor whose recorded white balance neutralises that light.
CameraNative cameraSeeing(float x, float y) {
    const std::array<float, 3> xyz{x / y, 1.0F, (1.0F - x - y) / y};
    const auto neutral = xyzToRec2020 * xyz;

    CameraNative camera;
    camera.asShotMultipliers =
        withGreenAtOne({1.0F / neutral[0], 1.0F / neutral[1], 1.0F / neutral[2]});
    camera.appliedMultipliers = camera.asShotMultipliers;
    return camera;
}

} // namespace

TEST_CASE("Published illuminants read back at their published temperatures", "[whitebalance]") {
    /// The check that pins the maths to something outside arraw. Illuminant A
    /// is a glowing filament, so it sits *on* the curve and its tint is zero;
    /// daylight sits a little beside it, which is what the small positive tint
    /// on D65 and D50 means.
    SECTION("Illuminant A, a tungsten filament at 2856 K") {
        const auto reading = asShotTemperature(cameraSeeing(0.44757F, 0.40745F));
        REQUIRE(std::abs(reading.kelvin - 2856.0F) < 5.0F);
        REQUIRE(std::abs(reading.tint) < 0.5F);
    }
    SECTION("D50, the printing standard at 5003 K") {
        const auto reading = asShotTemperature(cameraSeeing(0.34567F, 0.35850F));
        REQUIRE(std::abs(reading.kelvin - 5003.0F) < 5.0F);
        REQUIRE(reading.tint > 5.0F);
    }
    SECTION("D65, the daylight standard at 6504 K") {
        const auto reading = asShotTemperature(cameraSeeing(0.31272F, 0.32903F));
        REQUIRE(std::abs(reading.kelvin - 6504.0F) < 5.0F);
        REQUIRE(reading.tint > 5.0F);
    }
}

TEST_CASE("A temperature survives the trip into gains and back", "[whitebalance]") {
    const float kelvin = GENERATE(2000.0F, 3200.0F, 4500.0F, 5500.0F, 6500.0F, 9000.0F, 12000.0F);
    const float tint = GENERATE(-40.0F, 0.0F, 25.0F);

    CameraNative camera = cameraSeeing(0.31272F, 0.32903F);
    camera.asShotMultipliers = whiteBalanceGains(camera, {kelvin, tint});

    const auto reading = asShotTemperature(camera);

    CAPTURE(kelvin, tint, reading.kelvin, reading.tint);
    REQUIRE(std::abs(reading.kelvin - kelvin) < 1.0F);
    REQUIRE(std::abs(reading.tint - tint) < 0.1F);
}

TEST_CASE("A lower temperature cools the photograph", "[whitebalance]") {
    const CameraNative camera = cameraSeeing(0.31272F, 0.32903F);

    const auto warmLight = whiteBalanceGains(camera, {2800.0F, 0.0F});
    const auto coolLight = whiteBalanceGains(camera, {9000.0F, 0.0F});

    /// Telling arraw the light was warm makes it take red back out, which is
    /// the direction the slider moves in every other editor: left is bluer.
    REQUIRE(warmLight[0] < coolLight[0]);
    REQUIRE(warmLight[2] > coolLight[2]);
}

TEST_CASE("Tint moves across the temperature, not along it", "[whitebalance]") {
    const CameraNative camera = cameraSeeing(0.31272F, 0.32903F);

    const auto neutral = whiteBalanceGains(camera, {5500.0F, 0.0F});
    const auto magenta = whiteBalanceGains(camera, {5500.0F, 40.0F});
    const auto green = whiteBalanceGains(camera, {5500.0F, -40.0F});

    /// Green is pinned at 1, so "more magenta" shows up as red and blue rising
    /// together -- which is exactly what it is: everything except green.
    REQUIRE(magenta[0] > neutral[0]);
    REQUIRE(magenta[2] > neutral[2]);
    REQUIRE(green[0] < neutral[0]);
    REQUIRE(green[2] < neutral[2]);
}

TEST_CASE("Two sensors with the same matrix still balance differently", "[whitebalance]") {
    /// The reason CameraNative carries a daylight calibration at all. LibRaw
    /// normalises the camera matrix's rows and keeps the divisors separately,
    /// so these two cameras share a matrix and are not the same sensor. If the
    /// calibration were dropped, this test would see one camera.
    CameraNative even = cameraSeeing(0.31272F, 0.32903F);
    CameraNative uneven = even;
    uneven.daylightScale = {0.5F, 1.0F, 1.25F};

    const auto evenGains = whiteBalanceGains(even, {5000.0F, 0.0F});
    const auto unevenGains = whiteBalanceGains(uneven, {5000.0F, 0.0F});

    REQUIRE(std::abs(evenGains[0] - unevenGains[0]) > 0.1F);
    REQUIRE(std::abs(evenGains[2] - unevenGains[2]) > 0.1F);
}

TEST_CASE("Gains are rescaled about green", "[whitebalance]") {
    const auto gains = withGreenAtOne({4.0F, 2.0F, 1.0F});

    REQUIRE(gains[0] == 2.0F);
    REQUIRE(gains[1] == 1.0F);
    REQUIRE(gains[2] == 0.5F);
    REQUIRE_THROWS_AS(withGreenAtOne({1.0F, 0.0F, 1.0F}), std::invalid_argument);
}

TEST_CASE("A light outside the modelled range is brought inside it", "[whitebalance]") {
    /// The processing contract clamps whatever reaches it, so that no pixel
    /// maths depends on a caller having checked first (ADR 008). A sidecar
    /// written by another editor with a wider range is the reason this matters.
    const CameraNative camera = cameraSeeing(0.31272F, 0.32903F);

    REQUIRE(whiteBalanceGains(camera, {1500.0F, 0.0F}) ==
            whiteBalanceGains(camera, {warmestKelvin, 0.0F}));
    REQUIRE(whiteBalanceGains(camera, {30000.0F, 0.0F}) ==
            whiteBalanceGains(camera, {coolestKelvin, 0.0F}));
    REQUIRE(whiteBalanceGains(camera, {0.0F, 0.0F}) ==
            whiteBalanceGains(camera, {warmestKelvin, 0.0F}));
    REQUIRE(whiteBalanceGains(camera, {5500.0F, 400.0F}) ==
            whiteBalanceGains(camera, {5500.0F, tintLimit}));
}

TEST_CASE("A light that is not a number is refused", "[whitebalance]") {
    /// Clamping cannot rescue these: there is no nearest valid temperature to
    /// a NaN, and silently choosing one would put arbitrary colour in a file.
    const CameraNative camera = cameraSeeing(0.31272F, 0.32903F);
    const float notANumber = std::numeric_limits<float>::quiet_NaN();
    const float infinite = std::numeric_limits<float>::infinity();

    REQUIRE_THROWS_AS(whiteBalanceGains(camera, {notANumber, 0.0F}), std::invalid_argument);
    REQUIRE_THROWS_AS(whiteBalanceGains(camera, {5500.0F, notANumber}), std::invalid_argument);
    REQUIRE_THROWS_AS(whiteBalanceGains(camera, {infinite, 0.0F}), std::invalid_argument);
    REQUIRE_THROWS_AS(whiteBalanceGains(camera, {-infinite, 0.0F}), std::invalid_argument);
    REQUIRE_THROWS_AS(temperatureForGains(camera, {notANumber, 1.0F, 1.0F}), std::invalid_argument);
    REQUIRE_THROWS_AS(temperatureForGains(camera, {1.0F, 1.0F, 0.0F}), std::invalid_argument);
}
