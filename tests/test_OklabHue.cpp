#include "ColorAdjustments.h"
#include "ColorSpaces.h"
#include "OklabHue.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace arraw;
using namespace arraw::app;

/// The colours a hue slider paints (ADR 036).

namespace {

/// @brief Decodes an sRGB channel to linear.
float linearOf(double encoded) {
    return static_cast<float>(encoded <= 0.04045 ? encoded / 12.92
                                                 : std::pow((encoded + 0.055) / 1.055, 2.4));
}

/// @brief Reads a displayed colour back into Oklab.
Oklab oklabOf(const QColor& colour) {
    const Colour linear{linearOf(colour.redF()), linearOf(colour.greenF()),
                        linearOf(colour.blueF())};
    return toOklab(colorspaces::srgbToWorking * linear);
}

/// @brief Gives the distance between two hue angles, in degrees.
double hueDistance(double first, double second) {
    const double difference = std::fmod(std::abs(first - second), 360.0);
    return std::min(difference, 360.0 - difference);
}

} // namespace

TEST_CASE("A hue colour has the Oklab hue it stands for, at a middle lightness", "[app][hue]") {
    for (int degrees = 0; degrees < 360; degrees += 15) {
        CAPTURE(degrees);
        const Oklab lab = oklabOf(oklabHueColour(degrees));
        const double hue = std::atan2(lab.b, lab.a) * 180.0 / std::numbers::pi;
        // Within the rounding of 8-bit sRGB.
        CHECK(hueDistance(hue, degrees) < 3.0);
        CHECK(std::abs(lab.lightness - hueColourLightness) < 0.01F);
        CHECK(std::hypot(lab.a, lab.b) > 0.05F);
    }
}

TEST_CASE("Hue colours follow the grade's landmarks and close the wheel", "[app][hue]") {
    const QColor red = oklabHueColour(30.0);
    const QColor green = oklabHueColour(140.0);
    const QColor blue = oklabHueColour(260.0);
    CHECK(red.red() > red.green());
    CHECK(red.red() > red.blue());
    CHECK(green.green() > green.red());
    CHECK(green.green() > green.blue());
    CHECK(blue.blue() > blue.red());
    CHECK(blue.blue() > blue.green());
    CHECK(oklabHueColour(0.0) == oklabHueColour(360.0));
    CHECK(oklabHueColour(-30.0) == oklabHueColour(330.0));

    const QGradientStops stops = oklabHueStops(0.0, 360.0);
    REQUIRE(stops.size() == 37);
    CHECK(stops.front().first == 0.0);
    CHECK(stops.back().first == 1.0);
    CHECK(stops.front().second == stops.back().second);
    CHECK(stops[3].second == oklabHueColour(30.0));
}
