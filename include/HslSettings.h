#pragma once

namespace arraw {

/// @brief Weakest and strongest shift any HSL control arraw models.
///
/// Hue, saturation and luminance of every band share one range; what a full
/// shift means for each is the plan's business (ADR 027).
inline constexpr float weakestHslControl = -100.0F;

/// @copydoc weakestHslControl
inline constexpr float strongestHslControl = 100.0F;

/// @brief Hue, saturation and luminance shifts of one band of hues.
struct HueBand {
    /// @brief How far the band's hue turns; a hundred is thirty degrees.
    float hue = 0.0F;

    /// @brief How much colourfulness the band gains or loses.
    float saturation = 0.0F;

    /// @brief How much lighter or darker the band becomes.
    float luminance = 0.0F;

    friend bool operator==(const HueBand&, const HueBand&) = default;
};

/// @brief Per-hue colour adjustments over eight bands, in domain units.
///
/// The bands are Lightroom's, and sit at fixed hues around the colour wheel;
/// a colour is moved by the neighbouring bands in proportion to how close its
/// hue lies to each (ADR 027).
struct HslSettings {
    HueBand red;     ///< Band around 0 degrees.
    HueBand orange;  ///< Band around 30 degrees.
    HueBand yellow;  ///< Band around 60 degrees.
    HueBand green;   ///< Band around 120 degrees.
    HueBand aqua;    ///< Band around 180 degrees.
    HueBand blue;    ///< Band around 220 degrees.
    HueBand purple;  ///< Band around 280 degrees.
    HueBand magenta; ///< Band around 320 degrees.

    friend bool operator==(const HslSettings&, const HslSettings&) = default;
};

} // namespace arraw
