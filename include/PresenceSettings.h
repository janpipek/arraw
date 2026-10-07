#pragma once

namespace arraw {

/// @brief Weakest and strongest Texture, Clarity and Dehaze arraw models.
///
/// All three run from minus a hundred (softens, or adds haze) through zero
/// (leaves the photograph alone) to a hundred, as Lightroom's and `main`'s do
/// (ADR 041).
inline constexpr float weakestPresence = -100.0F;

/// @copydoc weakestPresence
inline constexpr float strongestPresence = 100.0F;

/// @brief Texture, Clarity and Dehaze: Lightroom's Presence controls that need a neighbourhood.
///
/// Each acts on a pixel's luminance relative to a blurred base of the
/// photograph around it (ADR 041): Texture to fine detail, a few sensor pixels
/// across; Clarity to mid-scale local contrast, a fraction of the long edge
/// across, mostly in the midtones; Dehaze takes off the veil it finds over the
/// darkest tones around a pixel (or adds one), with some colour. Saturation and Vibrance, the rest
/// of Lightroom's Presence, are colour controls and live in ::arraw::ColorSettings.
struct PresenceSettings {
    /// @brief How much fine detail is emphasised (positive) or smoothed (negative).
    float texture = 0.0F;

    /// @brief How much mid-scale local contrast is added (positive) or taken away (negative).
    float clarity = 0.0F;

    /// @brief How much atmospheric haze is removed (positive) or added (negative).
    float dehaze = 0.0F;

    friend bool operator==(const PresenceSettings&, const PresenceSettings&) = default;
};

/// @brief Whether any Presence control does anything, and so whether the chain reads a context.
[[nodiscard]] constexpr bool hasPresence(const PresenceSettings& settings) noexcept {
    return settings.texture != 0.0F || settings.clarity != 0.0F || settings.dehaze != 0.0F;
}

} // namespace arraw
