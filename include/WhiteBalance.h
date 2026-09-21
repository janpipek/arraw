#pragma once

#include <ColorEncoding.h>

namespace arraw {

/// @brief The colour of a light, as a photographer names it.
///
/// Two numbers, because one is not enough: real light sources sit *near* the
/// line of glowing-hot-object colours rather than on it. @ref kelvin says how
/// far along that line the light is, and @ref tint says how far off it.
struct ColourTemperature {
    /// @brief Temperature of the light, in kelvin.
    ///
    /// Low is warm (candles, tungsten), high is cool (shade, overcast) — the
    /// opposite of how "warm" and "cool" sound, because it names the
    /// temperature of the glowing object, not the mood of the picture.
    float kelvin = 5500.0F;

    /// @brief How far off that line the light sits.
    ///
    /// Positive says the light was greener than a glowing object can be, and
    /// so shifts the photograph toward magenta; negative does the reverse.
    /// Fluorescent and LED light is the usual reason it is not zero, and even
    /// daylight is a little off the line: measured here, D65 reads +9.8.
    float tint = 0.0F;

    friend bool operator==(const ColourTemperature&, const ColourTemperature&) = default;
};

/// @brief Computes the gains that neutralise a light, for one camera.
///
/// Every sensor answers a given light differently, so the same
/// @p temperature gives different gains on different bodies — which is exactly
/// why a temperature, rather than the gains, is what gets stored and copied
/// between photographs.
/// @param camera Sensor the gains are for.
/// @param temperature Light to neutralise.
/// @return Per-channel multipliers, normalised so green is 1.
[[nodiscard]] Gains whiteBalanceGains(const CameraNative& camera, ColourTemperature temperature);

/// @brief Rescales gains so green is 1, the convention everywhere in arraw.
///
/// Only the ratios between channels are a white balance; the overall level is
/// Exposure's business, so pinning green keeps the two from arguing.
/// @param gains Multipliers to rescale.
/// @return The same ratios, with green at 1.
[[nodiscard]] Gains withGreenAtOne(Gains gains);

/// @brief Reads back the light the camera says it saw.
///
/// The inverse of @ref whiteBalanceGains applied to what the file recorded, so
/// a photograph can open showing the temperature of its own scene rather than
/// an arbitrary starting number.
/// @param camera Sensor and the gains its file recorded.
/// @return The light those gains neutralise.
[[nodiscard]] ColourTemperature asShotTemperature(const CameraNative& camera);

} // namespace arraw
