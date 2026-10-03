#pragma once

namespace arraw {

/// @brief Darkest and lightest a hue band's grey may be mixed, as a weight.
///
/// A fully saturated colour of the band is made up to twice as bright, or
/// black, at the ends (ADR 027).
inline constexpr float darkestGrayMix = -100.0F;

/// @copydoc darkestGrayMix
inline constexpr float lightestGrayMix = 100.0F;

/// @brief Conversion to grey, and how each hue is weighed on the way, in domain units.
struct BlackAndWhiteSettings {
    /// @brief Whether the photograph is converted to grey.
    ///
    /// Replaces the colour controls rather than adding to them: a grey
    /// photograph has no colour left for Saturation, Vibrance or HSL to act on.
    bool convertToGrayscale = false;

    /// @brief How much lighter or darker reds come out; negative darkens.
    float red = 0.0F;

    /// @brief The same for oranges.
    float orange = 0.0F;

    /// @brief The same for yellows.
    float yellow = 0.0F;

    /// @brief The same for greens.
    float green = 0.0F;

    /// @brief The same for aquas.
    float aqua = 0.0F;

    /// @brief The same for blues.
    float blue = 0.0F;

    /// @brief The same for purples.
    float purple = 0.0F;

    /// @brief The same for magentas.
    float magenta = 0.0F;

    friend bool operator==(const BlackAndWhiteSettings&, const BlackAndWhiteSettings&) = default;
};

} // namespace arraw
