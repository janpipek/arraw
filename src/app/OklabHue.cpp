#include "OklabHue.h"

#include "ColorAdjustments.h"
#include "ColorGrading.h"
#include "ColorSpaces.h"

#include <ColorGradingSettings.h>

#include <algorithm>
#include <cmath>

namespace arraw::app {

namespace {

/// @brief Encodes a linear sRGB channel with the sRGB transfer function, held to 0 to 1.
double encodedSrgb(float linear) {
    const double value = std::clamp(static_cast<double>(linear), 0.0, 1.0);
    return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

/// @brief Checks that a linear sRGB colour is inside the gamut, within rounding.
bool inGamut(const Colour& colour) {
    constexpr float slack = 1e-4F;
    return std::ranges::all_of(
        colour, [](float channel) { return channel >= -slack && channel <= 1.0F + slack; });
}

} // namespace

QColor oklabHueColour(double degrees) {
    ColorGradingSettings settings;
    settings.shadows = {.hue =
                            static_cast<float>(std::fmod(std::fmod(degrees, 360.0) + 360.0, 360.0)),
                        .saturation = strongestGrade};
    const ZoneTint tint = colorGradingPlanFor(settings).shadowTint;
    // Less chroma until the colour fits: the hue matters more than the strength.
    Colour srgb{};
    for (float scale = 1.0F; scale > 0.0F; scale -= 0.05F) {
        srgb = colorspaces::workingToSrgb *
               fromOklab({hueColourLightness, scale * tint.a, scale * tint.b});
        if (inGamut(srgb)) {
            break;
        }
    }
    return QColor::fromRgbF(static_cast<float>(encodedSrgb(srgb[0])),
                            static_cast<float>(encodedSrgb(srgb[1])),
                            static_cast<float>(encodedSrgb(srgb[2])));
}

QGradientStops oklabHueStops(double from, double to, int count) {
    count = std::max(count, 2);
    QGradientStops stops;
    stops.reserve(count);
    for (int index = 0; index < count; ++index) {
        const double position = static_cast<double>(index) / (count - 1);
        stops.append({position, oklabHueColour(from + position * (to - from))});
    }
    return stops;
}

} // namespace arraw::app
