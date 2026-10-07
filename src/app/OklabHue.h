#pragma once

#include <QColor>
#include <QGradientStops>

namespace arraw::app {

/// @brief Oklab lightness the hue colours are shown at: a middle tone.
inline constexpr float hueColourLightness = 0.65F;

/// @brief Gives the colour a hue angle of the Oklab wheel stands for, for display.
///
/// The tint a Colour Grading zone at full saturation adds at that hue
/// (::arraw::colorGradingPlanFor, so its angle convention and chroma), on a
/// grey of ::hueColourLightness, converted to sRGB. Where that falls outside
/// sRGB the chroma is reduced rather than a channel clipped, so the hue stays
/// the one meant. Usable for any setting that is an Oklab hue angle.
/// @param degrees Hue angle; wrapped onto the wheel.
/// @return An opaque sRGB colour.
[[nodiscard]] QColor oklabHueColour(double degrees);

/// @brief Gives the stops of a gradient across a span of the Oklab wheel.
/// @param from Hue at position 0, in degrees.
/// @param to Hue at position 1, in degrees; 360 past @p from is the whole wheel.
/// @param count Number of stops, at least 2; the default puts one every ten degrees of a
/// whole wheel.
/// @return Stops at positions 0 to 1, evenly spaced in hue.
[[nodiscard]] QGradientStops oklabHueStops(double from, double to, int count = 37);

} // namespace arraw::app
