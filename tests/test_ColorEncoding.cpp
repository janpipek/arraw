#include "ColorEncoding.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace arraw;

namespace {

/// @brief Checks two colours agree to within a tolerance.
bool close(std::array<float, 3> actual, std::array<float, 3> expected, float tolerance = 1e-6F) {
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float difference = actual[channel] - expected[channel];
        if (difference > tolerance || difference < -tolerance) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("The identity transform leaves a colour alone", "[ColorEncoding]") {
    constexpr std::array<float, 3> colour{0.25F, 0.5F, 0.75F};

    REQUIRE(close(Matrix3::identity() * colour, colour));
}

TEST_CASE("A scale transform multiplies each channel on its own", "[ColorEncoding]") {
    constexpr auto gains = Matrix3::scale({2.0F, 1.0F, 0.5F});

    REQUIRE(close(gains * std::array<float, 3>{0.25F, 0.5F, 0.75F}, {0.5F, 0.5F, 0.375F}));
}

TEST_CASE("Composition applies the right-hand transform first", "[ColorEncoding]") {
    // Order matters: a white balance happens in the camera's space, so it must
    // be the inner transform of a composed camera-to-working matrix. Two
    // transforms that do not commute make the difference observable.
    constexpr auto gains = Matrix3::scale({2.0F, 1.0F, 1.0F});
    constexpr Matrix3 swapRedAndGreen{{0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F}};
    constexpr std::array<float, 3> colour{1.0F, 3.0F, 0.0F};

    const auto scaleThenSwap = swapRedAndGreen * gains;
    const auto swapThenScale = gains * swapRedAndGreen;

    REQUIRE(close(scaleThenSwap * colour, swapRedAndGreen * (gains * colour)));
    REQUIRE(close(scaleThenSwap * colour, {3.0F, 2.0F, 0.0F}));
    REQUIRE(close(swapThenScale * colour, {6.0F, 1.0F, 0.0F}));
}

TEST_CASE("Composing with the identity changes nothing", "[ColorEncoding]") {
    constexpr Matrix3 matrix{{0.7F, 0.2F, 0.1F, 0.0F, 1.0F, 0.0F, 0.1F, 0.1F, 0.8F}};

    REQUIRE(matrix * Matrix3::identity() == matrix);
    REQUIRE(Matrix3::identity() * matrix == matrix);
}

TEST_CASE("Only linear Rec.2020 is the working encoding", "[ColorEncoding]") {
    REQUIRE(isWorkingEncoding(ColorEncoding{NamedEncoding::LinearRec2020}));
    REQUIRE_FALSE(isWorkingEncoding(ColorEncoding{NamedEncoding::Srgb}));
    REQUIRE_FALSE(isWorkingEncoding(ColorEncoding{NamedEncoding::DisplayP3}));
    REQUIRE_FALSE(isWorkingEncoding(ColorEncoding{NamedEncoding::AdobeRgb}));
}

TEST_CASE("A camera-native encoding is not the working one, whatever its matrix",
          "[ColorEncoding]") {
    // Its samples are the sensor's primaries even when the matrix out of them
    // happens to be the identity, which is what every DNG fixture here has.
    CameraNative camera;
    REQUIRE(camera.toWorking == Matrix3::identity());

    REQUIRE_FALSE(isWorkingEncoding(ColorEncoding{camera}));
}

TEST_CASE("A camera-native encoding carries its own calibration", "[ColorEncoding]") {
    // The four fields exist because the matrix alone cannot describe a sensor:
    // LibRaw normalises the row scales out of it, and a file that declares no
    // neutral is decoded with gains the camera never recorded (ADR 007).
    CameraNative first;
    first.daylightScale = {1.0F, 1.0F, 1.0F};
    CameraNative second = first;
    second.daylightScale = {0.5F, 1.0F, 1.0F};

    REQUIRE(first != second);

    CameraNative substituted = first;
    substituted.asShotMultipliers = {1.0F, 1.0F, 1.0F};
    substituted.appliedMultipliers = {2.0F, 1.0F, 1.25F};

    REQUIRE(substituted.asShotMultipliers != substituted.appliedMultipliers);
}
