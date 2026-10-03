#pragma once

#include <BlackAndWhiteSettings.h>
#include <ColorEncoding.h>
#include <ColorSettings.h>
#include <HslSettings.h>

#include <array>
#include <cstddef>

namespace arraw {

/// @brief Number of bands the HSL and Black & White controls divide the hues into.
inline constexpr std::size_t hueBandCount = 8;

/// @brief One value per hue band, in the order red, orange, yellow, green, aqua, blue, purple,
/// magenta.
using BandValues = std::array<float, hueBandCount>;

/// @brief The colour block of a plan: saturation, vibrance, HSL and Black & White, resolved.
///
/// Part of the pointwise group (ADR 011, ADR 027). Each control that is left
/// at zero resolves to a flag that is off, so that the chain skips it outright
/// and default settings leave every pixel exactly as it was.
struct ColorAdjustmentPlan {
    /// @brief Whether Saturation changes anything.
    bool adjustsSaturation = false;

    /// @brief Saturation on the scale of its maths: minus one is grey, plus one doubles chroma.
    float saturation = 0.0F;

    /// @brief Whether Vibrance changes anything.
    bool adjustsVibrance = false;

    /// @brief Vibrance on the same scale as ::saturation.
    float vibrance = 0.0F;

    /// @brief Whether any HSL band changes anything.
    bool adjustsHsl = false;

    /// @brief Hue shift of each band, minus one to one: a full shift turns the hue by thirty
    /// degrees.
    BandValues hueShift{};

    /// @brief Saturation shift of each band, minus one to one: a full shift is half the saturation.
    BandValues bandSaturation{};

    /// @brief Luminance shift of each band, minus one to one: a full shift is half of white.
    BandValues bandLuminance{};

    /// @brief Whether the photograph is made grey, which replaces the other colour controls.
    bool convertsToGrayscale = false;

    /// @brief Weight of each band in the grey it is made into, minus a hundred to a hundred.
    ///
    /// Left in the setting's own units, as main's mixer reads them; the
    /// maths divides by a hundred.
    BandValues grayMix{};

    friend bool operator==(const ColorAdjustmentPlan&, const ColorAdjustmentPlan&) = default;
};

/// @brief Resolves the colour controls into the plan's colour block.
///
/// Out-of-range values are clamped rather than refused, like every setting
/// the pixel maths reads (ADR 008).
/// @param color Saturation and Vibrance.
/// @param hsl Per-band hue, saturation and luminance.
/// @param blackAndWhite Conversion to grey and its mix.
/// @return The block, with every control that is zero switched off.
/// @throws std::invalid_argument if a value is not finite.
[[nodiscard]] ColorAdjustmentPlan
colorAdjustmentPlanFor(const ColorSettings& color, const HslSettings& hsl,
                       const BlackAndWhiteSettings& blackAndWhite);

/// @brief A colour in Oklab: lightness and the two opponent axes.
struct Oklab {
    float lightness; ///< L, zero for black to about one for white.
    float a;         ///< Green to red.
    float b;         ///< Blue to yellow.
};

/// @brief Converts a linear Rec.2020 colour to Oklab.
///
/// Oklab is defined from linear Rec.709, so the colour is brought there first.
/// Negative channels, which a camera matrix can produce, keep their sign
/// through the cube root.
/// @param colour Colour in the working encoding.
/// @return The same colour in Oklab.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] Oklab toOklab(Colour colour);

/// @brief Converts an Oklab colour back to linear Rec.2020.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] Colour fromOklab(Oklab lab);

/// @brief Scales a colour's chroma uniformly, holding lightness and hue.
/// @param colour Colour in the working encoding.
/// @param amount Minus one for grey, zero for no change, plus one for double chroma.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] Colour applySaturation(Colour colour, float amount);

/// @brief Scales a colour's chroma, less the more of it there already is.
///
/// Muted colours move by nearly the whole amount and vivid ones by little, but
/// never by nothing, so a positive amount always nudges.
/// @param colour Colour in the working encoding.
/// @param amount Minus one to plus one, as for ::arraw::applySaturation.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] Colour applyVibrance(Colour colour, float amount);

/// @brief Shifts hue, saturation and luminance by the bands a colour's hue lies near.
///
/// Works on HSV, as Lightroom's panel does: each band weighs in with a
/// smoothstep over sixty degrees either side of its centre, and the weights
/// are normalised, so a colour between two bands takes a blend of both.
/// @param plan Colour block with the band shifts.
/// @param colour Colour in the working encoding.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] Colour applyHsl(const ColorAdjustmentPlan& plan, Colour colour);

/// @brief Makes a colour an achromatic grey, weighing it by its hue.
///
/// The grey starts as the colour's luminance. Each band then darkens or
/// lightens the greys made from colours of its hue, in proportion to the
/// colour's saturation, so neutrals never move and an all-zero mix is plain
/// luminance.
/// @param mix Weight of each band, minus a hundred to a hundred.
/// @param colour Colour in the working encoding.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] Colour applyBlackAndWhite(const BandValues& mix, Colour colour);

/// @brief Applies the colour block to one colour: grey, or HSL then saturation then vibrance.
///
/// Black & White replaces the colour controls, which have nothing to act on
/// in a grey photograph (ADR 027).
/// @param plan Colour block.
/// @param colour Colour in the working encoding, after tone and the shoulder.
/// @return The adjusted colour; exactly @p colour when the block is all off.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline Colour adjustColor(const ColorAdjustmentPlan& plan, Colour colour) {
    if (plan.convertsToGrayscale) {
        return applyBlackAndWhite(plan.grayMix, colour);
    }
    if (plan.adjustsHsl) {
        colour = applyHsl(plan, colour);
    }
    if (plan.adjustsSaturation) {
        colour = applySaturation(colour, plan.saturation);
    }
    if (plan.adjustsVibrance) {
        colour = applyVibrance(colour, plan.vibrance);
    }
    return colour;
}

} // namespace arraw
