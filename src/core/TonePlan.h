#pragma once

#include "ColorSpaces.h"

#include <ColorEncoding.h>
#include <ToneSettings.h>

#include <algorithm>
#include <cmath>

namespace arraw {

/// @brief Perceptual coordinate the tone controls act in.
///
/// Tone shaping happens on `y^(1/2.2)` rather than on scene-linear luminance:
/// linear 0.25 is upper-midtone grey, not the dark quarter, so a control that
/// acted there would put its whole range in the highlights (ADR 010).
/// @param luminance Linear luminance, zero or above.
/// @return The same brightness, perceptually spaced.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline float toPerceptual(float luminance) {
    return std::pow(luminance, 1.0F / 2.2F);
}

/// @brief Returns a perceptual value to scene-linear luminance.
/// @param value Perceptually spaced brightness.
/// @return The linear luminance it stands for.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline float toLinear(float value) {
    return std::pow(value, 2.2F);
}

/// @brief Middle grey in the perceptual coordinate, which Contrast pivots on.
///
/// An eighteen percent grey card, encoded: `0.18^(1/2.2)`, to the nearest
/// float, so that a contrast control leaves the value a photographer metered
/// for exactly where it was.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
inline constexpr float greyPivot = 0.45865646F;

/// @brief Smooth rise from zero to one between two edges.
/// @param first Edge below which the result is zero.
/// @param last Edge above which it is one.
/// @param value Where to evaluate it.
/// @return The eased fraction, never outside zero to one.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float smoothstep(float first, float last, float value) {
    const float t = std::clamp((value - first) / (last - first), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

/// @brief Weight of the Shadows region: zero at black, zero by the midtones.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float shadowWeight(float value) {
    return smoothstep(0.0F, 0.3F, value) * (1.0F - smoothstep(0.3F, 0.6F, value));
}

/// @brief Weight of the Highlights region, reaching a little past white.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float highlightWeight(float value) {
    return smoothstep(0.4F, 0.75F, value) * (1.0F - smoothstep(0.75F, 1.2F, value));
}

/// @brief Weight of the black end, full at black and gone by the shadows.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float blackWeight(float value) {
    return 1.0F - smoothstep(0.0F, 0.35F, value);
}

/// @brief Weight of the white end, full at white and above.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr float whiteWeight(float value) {
    return smoothstep(0.6F, 1.0F, value);
}

/// @brief How far Shadows and Highlights move their region, at full setting.
///
/// In the perceptual coordinate. A smoothstep window's slope is at most
/// `1.5 / width`, so this reach perturbs the tone scale's slope by at most
/// 0.6 -- and the widest opposing pair, Shadows against Blacks, stays under
/// one, which is what keeps the scale from folding back (ADR 013).
inline constexpr float regionalReach = 0.12F;

/// @brief How far Blacks and Whites move their end, at full setting.
inline constexpr float endpointReach = 0.08F;

/// @brief Basic Tone in setting units, each finite and inside its range.
///
/// What ::arraw::toneAmountsOf makes of ::arraw::ToneSettings, and what
/// ::arraw::resolveTone resolves into a ::arraw::TonePlan.
struct ToneAmounts {
    float exposure = 0.0F;   ///< Exposure, in stops.
    float contrast = 0.0F;   ///< Contrast, minus a hundred to a hundred.
    float highlights = 0.0F; ///< Highlights, minus a hundred to a hundred.
    float shadows = 0.0F;    ///< Shadows, minus a hundred to a hundred.
    float whites = 0.0F;     ///< Whites, minus a hundred to a hundred.
    float blacks = 0.0F;     ///< Blacks, minus a hundred to a hundred.
};

/// @brief Exposure and Basic Tone, resolved: the coefficients the chain applies.
///
/// Not the shoulder, which stays global and comes after the curve input tap
/// (ADR 044).
struct TonePlan {
    /// @brief Linear gain that the Exposure setting asks for.
    float exposureGain = 1.0F;

    /// @brief Whether any tone control asks for the scale to be shaped.
    ///
    /// Nothing set resolves to `false`, and the chain skips the crossing into
    /// the perceptual coordinate and back -- which costs two powers, and would
    /// return a value a hair away from the one it was given. A setting that is
    /// off falls out in the plan rather than as a test inside the loop
    /// (ADR 011).
    bool shapesTone = false;

    /// @brief Exponent the tone scale is raised to about middle grey.
    ///
    /// The Contrast setting resolved into the perceptual slope at the pivot.
    /// One leaves the tone scale alone.
    float contrastSlope = 1.0F;

    /// @brief What the raised value is multiplied by to bring grey back.
    ///
    /// `greyPivot^(1 - contrastSlope)`, worked out once: a power law through
    /// the pivot written as a multiply and a power rather than as a divide, a
    /// power and a multiply. With no contrast asked for it is exactly one, so
    /// the stage returns precisely the value it was given.
    float contrastScale = 1.0F;

    /// @brief How far the dark tones move, at the peak of their region.
    ///
    /// In the perceptual coordinate, where the controls act. The window each
    /// shift is spread over lives in the chain; the plan carries how far.
    float shadowShift = 0.0F;

    /// @copydoc shadowShift
    float highlightShift = 0.0F;

    /// @copydoc shadowShift
    float blackShift = 0.0F;

    /// @copydoc shadowShift
    float whiteShift = 0.0F;

    friend bool operator==(const TonePlan&, const TonePlan&) = default;
};

// The resolution functions below take a finite setting already inside its
// range and never throw: the planner calls them once for the photograph, and a
// pixel may call them for its own setting (ADR 044). Each body is the
// expression the planner has always used, so the results are the same bits.

/// @brief Resolves Exposure into a linear gain.
/// @param exposure Exposure in stops, finite and inside its range.
/// @return The gain.
[[nodiscard]] inline float exposureGainFor(float exposure) noexcept {
    return std::exp2(exposure);
}

/// @brief Resolves Contrast into how steeply the tone scale rises through middle grey.
///
/// The slider is the exponent's scale: a hundred is a perceptual slope of
/// 1.41 at the pivot, minus a hundred its reciprocal, so equal moves in
/// either direction undo one another (ADR 013).
/// @param contrast Contrast setting, finite and inside its range.
/// @return The exponent the tone scale is raised to.
[[nodiscard]] inline float contrastSlopeFor(float contrast) noexcept {
    return std::exp2(contrast / (2.0F * steepestContrast));
}

/// @brief Resolves a contrast slope into what the raised value is multiplied by.
/// @param slope Exponent from ::arraw::contrastSlopeFor.
/// @return `greyPivot^(1 - slope)`.
[[nodiscard]] inline float contrastScaleFor(float slope) noexcept {
    return std::pow(greyPivot, 1.0F - slope);
}

/// @brief Resolves Shadows or Highlights into how far the region moves.
///
/// Shadows and Highlights reach further than Blacks and Whites, because a
/// region has room to move and an end does not; the amounts are chosen so that
/// no two controls together can fold the tone scale back on itself (ADR 013).
/// @param setting Setting, finite and inside its range.
/// @return The shift the chain adds at the peak of the region.
[[nodiscard]] constexpr float regionalShiftFor(float setting) noexcept {
    return regionalReach * setting / strongestToneControl;
}

/// @brief Resolves Blacks or Whites into how far the end moves.
/// @param setting Setting, finite and inside its range.
/// @return The shift the chain adds at the peak of the end.
[[nodiscard]] constexpr float endpointShiftFor(float setting) noexcept {
    return endpointReach * setting / strongestToneControl;
}

/// @brief Resolves Basic Tone into the coefficients of the chain.
/// @param amounts Settings, finite and inside their ranges.
/// @return The tone plan.
[[nodiscard]] inline TonePlan resolveTone(const ToneAmounts& amounts) noexcept {
    const float slope = contrastSlopeFor(amounts.contrast);
    return {.exposureGain = exposureGainFor(amounts.exposure),
            .shapesTone = amounts.contrast != 0.0F || amounts.shadows != 0.0F ||
                          amounts.highlights != 0.0F || amounts.blacks != 0.0F ||
                          amounts.whites != 0.0F,
            .contrastSlope = slope,
            .contrastScale = contrastScaleFor(slope),
            .shadowShift = regionalShiftFor(amounts.shadows),
            .highlightShift = regionalShiftFor(amounts.highlights),
            .blackShift = endpointShiftFor(amounts.blacks),
            .whiteShift = endpointShiftFor(amounts.whites)};
}

/// @brief Shapes one luminance through the tone controls, in their fixed order.
///
/// Contrast is a straight line in log--log through the grey pivot: monotone
/// everywhere, never negative, and continuing smoothly past white rather than
/// flattening into it. The four regional controls are smooth windows added on
/// top, each fading out where the next one's territory begins (ADR 013).
/// @param tone Resolved tone controls.
/// @param luminance Linear luminance to shape.
/// @return The shaped linear luminance.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline float shapeLuminance(const TonePlan& tone, float luminance) {
    float value = toPerceptual(luminance);
    value = tone.contrastScale * std::pow(value, tone.contrastSlope);

    // The global shape first, then the regions, then the ends -- each reading
    // what the one before it left, and each moving its region by little enough
    // that no combination can fold the tone scale back on itself (ADR 013).
    value += tone.shadowShift * shadowWeight(value);
    value += tone.highlightShift * highlightWeight(value);
    value += tone.blackShift * blackWeight(value);
    value += tone.whiteShift * whiteWeight(value);
    return toLinear(std::max(value, 0.0F));
}

/// @brief Luminance at or below which tone treats a colour as black.
///
/// Below it a luminance cannot be told from black, and the ratio the tone
/// controls scale the colour by could be denormal or overflow, which GPUs flush
/// where CPUs do not. Both backends therefore take the lifted branch on the same
/// side of this value, a normal float far under anything a photograph holds.
/// NaN fails the comparison and lifts too. Mirrored by `liftedBlackThreshold` in
/// `src/gpu/shaders/develop.frag`.
inline constexpr float liftedBlackThreshold = 1.0e-20F;

/// @brief Applies the tone controls to a colour, through its luminance.
///
/// Tone shapes brightness and the colour follows by the ratio, so hue and
/// saturation come through untouched and colour work stays in Oklab where
/// ADR 010 puts it. A colour with no brightness has no ratio to scale by, and
/// takes the shaped value neutrally — which is what a lifted black is.
/// @param tone Resolved tone controls.
/// @param colour Colour in the working encoding.
/// @return The colour with its tone shaped.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr Colour shapeTone(const TonePlan& tone, Colour colour) {
    if (!tone.shapesTone) {
        return colour;
    }

    const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                            colorspaces::workingLuminance[1] * colour[1] +
                            colorspaces::workingLuminance[2] * colour[2];
    if (!(luminance > liftedBlackThreshold)) {
        const float lifted = shapeLuminance(tone, 0.0F);
        return {lifted, lifted, lifted};
    }

    const float ratio = shapeLuminance(tone, luminance) / luminance;
    return {colour[0] * ratio, colour[1] * ratio, colour[2] * ratio};
}

/// @brief Rolls a colour's brightest values toward white rather than clipping.
///
/// The shoulder that ends the chain (ADR 010), as a bend in luminance: below
/// the knee nothing moves, above it the excess is compressed ever harder, so
/// the result approaches white without ever reaching it and two values that
/// would both have clipped stay apart. The bend starts at slope one, so a
/// gradient shows no edge where it begins.
///
/// Colour fades with it. Something genuinely overexposed loses colour as it
/// brightens, so as a value is pulled down its chroma is faded toward the
/// neutral of the same luminance — the "path to white". Brightness is the
/// shoulder's, colour is the fade's: the luminance that comes out is the one
/// the bend asked for either way.
/// @param shoulderKnee Luminance at which highlights begin to roll (::arraw::shoulderKneeFor).
/// @param colour Colour in the working encoding, possibly above white.
/// @return The colour with its highlights rolled.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] constexpr Colour rollHighlights(float shoulderKnee, Colour colour) {
    const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                            colorspaces::workingLuminance[1] * colour[1] +
                            colorspaces::workingLuminance[2] * colour[2];
    if (!(luminance > shoulderKnee)) {
        return colour;
    }

    const float headroom = 1.0F - shoulderKnee;
    const float above = (luminance - shoulderKnee) / headroom;
    const float rolled = shoulderKnee + headroom * (above / (1.0F + above));

    const float ratio = rolled / luminance;
    // Chroma fades as the square of how far the value was pulled down, times
    // its square root: gentle while the roll is gentle, complete by the time
    // the value is being crushed into white.
    const float chroma = ratio * std::sqrt(ratio);
    return {rolled + chroma * (colour[0] * ratio - rolled),
            rolled + chroma * (colour[1] * ratio - rolled),
            rolled + chroma * (colour[2] * ratio - rolled)};
}

/// @brief Checks the tone settings are finite and clamps them into their ranges.
/// @param settings Tone adjustments to read.
/// @return The exposure and Basic Tone controls, ready for ::arraw::resolveTone.
/// @throws std::invalid_argument if one is not finite; the first in the order
/// exposure, contrast, shadows, highlights, blacks, whites is the one named.
[[nodiscard]] ToneAmounts toneAmountsOf(const ToneSettings& settings);

/// @brief Resolves tone settings into the coefficients of the chain.
/// @param settings Tone adjustments to resolve.
/// @return Exposure gain and tone coefficients for the pointwise chain.
/// @throws std::invalid_argument if a tone setting is not finite.
[[nodiscard]] TonePlan tonePlanFor(const ToneSettings& settings);

/// @brief Works out where the highlight roll-off bends.
///
/// The amount is where the knee sits: none leaves it out of reach, full brings
/// it down to half of white. The plan carries the knee rather than the amount
/// so that the chain has a comparison to make rather than a setting to
/// interpret (ADR 011).
/// @param settings Settings to resolve.
/// @return The knee, in linear luminance; infinity for no roll-off.
/// @throws std::invalid_argument if the setting is not finite.
[[nodiscard]] float shoulderKneeFor(const ToneSettings& settings);

} // namespace arraw
