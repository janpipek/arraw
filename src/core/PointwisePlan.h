#pragma once

#include "ColorAdjustments.h"
#include "ColorSpaces.h"
#include "LocalPlan.h"
#include "Presence.h"
#include "ToneCurve.h"
#include "TonePlan.h"

#include <ColorEncoding.h>
#include <Develop.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <tuple>

namespace arraw {

/// @brief The pointwise stages of a plan, resolved, in the order the chain applies them.
///
/// ADR 011's pointwise group as one block: white balance and the matrix,
/// Exposure and Basic Tone, Presence, the curves, the shoulder and the colour
/// controls. Settings that are switched off resolve to a value that costs
/// nothing here rather than to a test inside the per-pixel loop.
struct PointwisePlan {
    /// @brief Source primaries into the working space, white balance included.
    ///
    /// White balance, the camera matrix, and any change of primaries are all
    /// linear, so they compose into one transform and cost one multiply per
    /// pixel between them.
    Matrix3 toWorking = Matrix3::identity();

    /// @brief The local adjustments, resolved against the size of the source (ADR 044).
    ///
    /// The masks that do something, the globals they add to and the controls they touch. Empty
    /// for a state without masks, and for the overloads of ::arraw::planFor that know no size,
    /// as Presence is; the chain then has every pixel take the photograph's own amounts. Pixels
    /// under a mask get amounts of their own from ::arraw::amountsAt.
    LocalPlan local{};

    /// @brief Exposure and Basic Tone, resolved.
    TonePlan tone{};

    /// @brief Texture, Clarity and Dehaze, resolved, and the context they read.
    ///
    /// After Basic Tone and before the curves, as Lightroom's Presence sits in
    /// its Basic panel (ADR 041). The context they read is worked out from the
    /// pointwise pass's input and from fields of this block alone, so white
    /// balance and exposure never recompute it. Resolved only by the overloads
    /// of ::arraw::planFor that know the source's size; every control at zero
    /// is the default, and the chain reads no context.
    PresencePlan presence{};

    /// @brief Luma, red, green and blue tone curves, resolved.
    ///
    /// They follow Basic Tone and precede the shoulder, so the shoulder still
    /// catches whatever a curve lifts past white; a curve that is the
    /// identity is a flag that is off (ADR 011).
    ToneCurvePlan toneCurves{};

    /// @brief Luminance at which highlights begin to roll toward white.
    ///
    /// Where the Filmic Highlights amount puts the bend, in linear luminance:
    /// a stronger amount brings the knee down out of white. A photograph asked
    /// for no roll-off resolves to a knee no luminance reaches, so the chain
    /// costs a comparison rather than a branch on a setting (ADR 011).
    float shoulderKnee = std::numeric_limits<float>::infinity();

    /// @brief Saturation, vibrance, HSL, Black & White and Colour Grading, resolved.
    ///
    /// The last of the pointwise stages: it follows the shoulder, as the
    /// colour controls did on main, and every control left at zero is a flag
    /// that is off (ADR 027). Colour Grading ends it (ADR 034).
    ColorAdjustmentPlan colorAdjustments{};

    friend bool operator==(const PointwisePlan&, const PointwisePlan&) = default;
};

/// @brief The values of a plan that a pixel applies as its own: the seam for per-pixel amounts.
///
/// The photograph's (::arraw::globalAmountsOf) for a pixel no mask reaches, and otherwise the
/// pixel's own, resolved by ::arraw::amountsAt (ADR 044).
struct PixelAmounts {
    /// @brief Whether the pixel takes the relative Temperature and Tint gain.
    bool balances = false;

    /// @brief Relative Temperature and Tint at the pixel, as a gain per working channel
    /// (::arraw::relativeBalanceGainFor); not read unless @ref balances.
    Colour balance{1.0F, 1.0F, 1.0F};

    TonePlan tone{};            ///< Exposure and Basic Tone at the pixel.
    PresenceAmounts presence{}; ///< Texture, Clarity and Dehaze at the pixel.
    ChromaAmounts chroma{};     ///< Saturation and Vibrance at the pixel.

    friend bool operator==(const PixelAmounts&, const PixelAmounts&) = default;
};

/// @brief Gives the amounts every pixel has when nothing is local.
/// @param plan Resolved settings.
/// @return The photograph's tone, Presence and chroma amounts.
[[nodiscard]] constexpr PixelAmounts globalAmountsOf(const PointwisePlan& plan) noexcept {
    return {.tone = plan.tone,
            .presence = plan.presence.amounts,
            .chroma = plan.colorAdjustments.chroma};
}

/// @brief Gives the amounts of the pixel at a column and row of the source.
///
/// For each control the sum of the masks' weighted amounts, summed in list order and in float
/// (::arraw::localSumsAt); where the sum is exactly zero the pixel takes the plan's global
/// value, as resolved, and so is bit-identical to a render without masks. Otherwise the global
/// value in setting units is added, the result clamped once to the global control's range, and
/// resolved by the expression the global setting uses (ADR 044, section 2). A pixel shapes tone
/// when the plan does or any of its five tone sums is non-zero. Relative Temperature and Tint
/// resolve to a gain, when either sum is non-zero.
/// @param plan Resolved settings.
/// @param column Column of the pixel in the source the plan was resolved for.
/// @param row Row of the pixel.
/// @param coverage The pixel's coverage codes, for the brush masks of the plan.
/// @return The pixel's amounts; the plan's own for a plan without masks.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline PixelAmounts amountsAt(const PointwisePlan& plan, std::uint32_t column,
                                            std::uint32_t row, const PixelCoverage& coverage) {
    PixelAmounts at = globalAmountsOf(plan);
    const LocalPlan& local = plan.local;
    if (local.empty()) {
        return at;
    }
    // The centre of the pixel, in source pixels.
    const LocalAmounts sums = localSumsAt(local, static_cast<float>(column) + 0.5F,
                                          static_cast<float>(row) + 0.5F, coverage);
    const auto sum = [&sums](LocalControl control) { return sums[indexOf(control)]; };
    // The global value and the pixel's sum, clamped once to the global control's range.
    const auto effective = [&](LocalControl control, float least, float most) {
        return std::clamp(local.global[indexOf(control)] + sum(control), least, most);
    };

    const float temperature = sum(LocalControl::RelativeTemperature);
    const float tint = sum(LocalControl::RelativeTint);
    if (temperature != 0.0F || tint != 0.0F) {
        at.balances = true;
        at.balance =
            relativeBalanceGainFor(std::clamp(temperature, -static_cast<float>(localControlLimit),
                                              static_cast<float>(localControlLimit)),
                                   std::clamp(tint, -static_cast<float>(localControlLimit),
                                              static_cast<float>(localControlLimit)));
    }

    TonePlan& tone = at.tone;
    if (sum(LocalControl::Exposure) != 0.0F) {
        tone.exposureGain =
            exposureGainFor(effective(LocalControl::Exposure, darkestExposure, brightestExposure));
    }
    if (sum(LocalControl::Contrast) != 0.0F) {
        tone.contrastSlope =
            contrastSlopeFor(effective(LocalControl::Contrast, flattestContrast, steepestContrast));
        tone.contrastScale = contrastScaleFor(tone.contrastSlope);
        tone.shapesTone = true;
    }
    if (sum(LocalControl::Highlights) != 0.0F) {
        tone.highlightShift = regionalShiftFor(
            effective(LocalControl::Highlights, weakestToneControl, strongestToneControl));
        tone.shapesTone = true;
    }
    if (sum(LocalControl::Shadows) != 0.0F) {
        tone.shadowShift = regionalShiftFor(
            effective(LocalControl::Shadows, weakestToneControl, strongestToneControl));
        tone.shapesTone = true;
    }
    if (sum(LocalControl::Whites) != 0.0F) {
        tone.whiteShift = endpointShiftFor(
            effective(LocalControl::Whites, weakestToneControl, strongestToneControl));
        tone.shapesTone = true;
    }
    if (sum(LocalControl::Blacks) != 0.0F) {
        tone.blackShift = endpointShiftFor(
            effective(LocalControl::Blacks, weakestToneControl, strongestToneControl));
        tone.shapesTone = true;
    }

    if (sum(LocalControl::Texture) != 0.0F) {
        at.presence.texture =
            presenceAmountFor(effective(LocalControl::Texture, weakestPresence, strongestPresence));
    }
    if (sum(LocalControl::Clarity) != 0.0F) {
        at.presence.clarity =
            presenceAmountFor(effective(LocalControl::Clarity, weakestPresence, strongestPresence));
    }
    if (sum(LocalControl::Dehaze) != 0.0F) {
        at.presence.dehaze =
            presenceAmountFor(effective(LocalControl::Dehaze, weakestPresence, strongestPresence));
    }

    if (sum(LocalControl::Saturation) != 0.0F || sum(LocalControl::Vibrance) != 0.0F) {
        at.chroma = chromaAmountsFor(
            effective(LocalControl::Saturation, weakestSaturation, strongestSaturation),
            effective(LocalControl::Vibrance, weakestSaturation, strongestSaturation));
    }
    return at;
}

/// @brief Luminance below which the luma curve stops dividing by it.
///
/// A curve that lifts black climbs out of zero infinitely steeply in linear
/// light, because the lift is made in the perceptual coordinate. Divided by
/// the luminance, the part of such a curve above its lift grows without bound
/// as the luminance falls, and a colour outside the working gamut with almost
/// no luminance but large channels would be scaled by thousands. Below this
/// luminance the ratio is therefore held at its value here, and the curve
/// continues as the straight line, in linear light, from its lift to its value
/// here, on through zero into negative luminance. It is 2^-14, fourteen stops
/// under white and the floor of a 14-bit raw file; on a curve lifting black to
/// 0.2 the line and the curve differ there by under 3% of the value. Mirrored
/// by `curveRatioFloor` in `src/gpu/shaders/develop.frag`.
inline constexpr float curveRatioFloor = 0x1p-14F;

/// @brief Applies the tone curves to a colour: luminance first, then each channel.
///
/// The luma curve acts as the tone controls do: on the luminance in the
/// perceptual coordinate, with the colour following by the ratio, so hue and
/// saturation come through. A curve that lifts black is split in two: its
/// value at black, the lift, is added to every channel as a neutral, and only
/// the rest, `toLinear(curve(x)) - lift`, scales the colour by the ratio. A grey
/// therefore lands exactly on the curve, while a colour near black keeps its
/// hue and the ratio stays bounded, with ::arraw::curveRatioFloor holding it
/// for the darkest and for negative luminances, continuously across zero.
/// A NaN luminance has no ratio at all, and takes the lift as a neutral.
///
/// The red, green and blue curves then act on their channels alone, each in
/// the perceptual coordinate, and may shift hue: that is their purpose. A
/// negative channel, which only a colour outside the working gamut has, cannot
/// enter the perceptual coordinate; it is moved by the curve's lift instead,
/// `value + toLinear(curve(0))`, which meets the curve continuously at zero and
/// keeps the colour as far outside the gamut as it was. A NaN channel takes
/// the curve's value at black, as zero does. A channel whose curve is inactive
/// is returned untouched.
/// @param curves Resolved curves.
/// @param colour Colour in the working encoding.
/// @return The colour with every active curve applied; the colour itself when none is.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline Colour applyToneCurves(const ToneCurvePlan& curves, Colour colour) {
    if (curves.luma.active) {
        const float lift = toLinear(std::max(evaluateCurve(curves.luma, 0.0F), 0.0F));
        const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                                colorspaces::workingLuminance[1] * colour[1] +
                                colorspaces::workingLuminance[2] * colour[2];
        // A NaN fails both comparisons.
        if (!(luminance > curveRatioFloor) && !(luminance <= curveRatioFloor)) {
            colour = {lift, lift, lift};
        } else {
            const float anchor = luminance > curveRatioFloor ? luminance : curveRatioFloor;
            const float shaped =
                toLinear(std::max(evaluateCurve(curves.luma, toPerceptual(anchor)), 0.0F));
            const float ratio = (shaped - lift) / anchor;
            colour = {colour[0] * ratio + lift, colour[1] * ratio + lift, colour[2] * ratio + lift};
        }
    }
    const auto channel = [](const CurvePlan& curve, float value) {
        if (!curve.active) {
            return value;
        }
        if (value > 0.0F) {
            return toLinear(std::max(evaluateCurve(curve, toPerceptual(value)), 0.0F));
        }
        const float lift = toLinear(std::max(evaluateCurve(curve, 0.0F), 0.0F));
        return value < 0.0F ? value + lift : lift;
    };
    return {channel(curves.red, colour[0]), channel(curves.green, colour[1]),
            channel(curves.blue, colour[2])};
}

/// @brief Applies the pointwise stages up to the curve input tap, in their fixed order.
///
/// The first half of ::arraw::developPixel, which calls this rather than
/// repeating it, so that ::arraw::Tap::CurveInput and its position in the
/// chain cannot drift apart (ADR 011): white balance and the matrix, exposure,
/// Basic Tone, then Texture, Clarity and Dehaze, which Lightroom counts as
/// Basic too (ADR 041). A pixel under local Temperature or Tint takes their gain right after the
/// matrix (ADR 044). What comes out is what the tone curves take in.
/// @param plan Resolved settings.
/// @param at The amounts that are the pixel's own: tone, Presence and chroma.
/// @param colour Source colour, in the encoding the plan was built for.
/// @param context The Presence context at the pixel; not read when Presence is off.
/// @return The colour at the curve input, in the working encoding; a sample
/// encodes it into ::arraw::perceptualEncoding afterwards (see
/// ::arraw::toPerceptualSigned).
///
/// Mirrored by `src/gpu/shaders/develop.frag` up to its `probeAfterTone`
/// stop, which ::arraw::probeFor names for this tap.
[[nodiscard]] inline Colour developToCurveInput(const PointwisePlan& plan, const PixelAmounts& at,
                                                Colour colour, const PixelContext& context) {
    // Measured on the source colour, before white balance and exposure, which
    // therefore cannot change the detail the Presence controls see.
    const float logLuminance =
        plan.presence.active() ? presenceLogLuminance(plan.presence, colour) : 0.0F;
    colour = plan.toWorking * colour;
    if (at.balances) {
        colour = {colour[0] * at.balance[0], colour[1] * at.balance[1], colour[2] * at.balance[2]};
    }
    colour = {colour[0] * at.tone.exposureGain, colour[1] * at.tone.exposureGain,
              colour[2] * at.tone.exposureGain};
    colour = shapeTone(at.tone, colour);
    return applyPresence(plan.presence, at.presence, colour, logLuminance, context);
}

/// @brief Applies the pointwise stages to one colour, in their fixed order.
///
/// The order lives here and nowhere else, so that it can be read in one place
/// and tested without a buffer. A fragment shader's `main` mirrors this
/// sequence by hand, and per-stage comparisons hold the two together (ADR 011).
/// @param plan Resolved settings.
/// @param at The amounts that are the pixel's own: tone, Presence and chroma.
/// @param colour Source colour, in the encoding the plan was built for.
/// @param context What the chain knows of the pixel's surroundings: the
/// Presence context at it (ADR 011, ADR 041); not read when Presence is off.
/// @return The developed colour, in the working encoding.
///
/// Not `constexpr`: the colour block's Oklab maths takes cube roots, which
/// the standard does not allow in a constant expression.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline Colour developPixel(const PointwisePlan& plan, const PixelAmounts& at,
                                         Colour colour, const PixelContext& context) {
    colour = developToCurveInput(plan, at, colour, context);
    colour = applyToneCurves(plan.toneCurves, colour);
    colour = rollHighlights(plan.shoulderKnee, colour);
    return adjustColor(plan.colorAdjustments, at.chroma, colour);
}

/// @brief Applies the pointwise stages to one colour, with the plan's own amounts.
///
/// The global case: every pixel has the amounts of the whole photograph, which
/// ::arraw::globalAmountsOf gives. A loop over many pixels works them out once
/// and calls the overload above.
/// @param plan Resolved settings.
/// @param colour Source colour, in the encoding the plan was built for.
/// @param context What the chain knows of the pixel's surroundings; not read when Presence is off.
/// @return The developed colour, in the working encoding.
[[nodiscard]] inline Colour developPixel(const PointwisePlan& plan, Colour colour,
                                         const PixelContext& context) {
    return developPixel(plan, globalAmountsOf(plan), colour, context);
}

/// @brief Applies the pointwise stages to one colour of a plan with Presence off.
///
/// For callers that have no context, which a plan with Texture, Clarity or
/// Dehaze on needs: there is no neutral context to stand in for one (a zero
/// base would read as every pixel standing far above its surroundings), so
/// asking this of such a plan is a precondition violation, asserted.
/// @param plan Resolved settings, with Presence off.
/// @param colour Source colour, in the encoding the plan was built for.
/// @return The developed colour, in the working encoding.
[[nodiscard]] inline Colour developPixel(const PointwisePlan& plan, Colour colour) {
    assert(!plan.presence.active() && "a plan with Presence on needs the pixel's context");
    return developPixel(plan, colour, PixelContext{});
}

/// @brief Applies the pointwise stages up to a tap, in their fixed order.
///
/// Taps are named positions inside the chain (ADR 011), and each is a prefix
/// of ::arraw::developPixel that it shares rather than repeats.
/// @param plan Resolved settings.
/// @param at The amounts that are the pixel's own: tone, Presence and chroma.
/// @param colour Source colour, in the encoding the plan was built for.
/// @param tap Where to stop.
/// @param context The Presence context at the pixel, as for ::arraw::developPixel.
/// @return The colour at @p tap, still in the working encoding.
[[nodiscard]] inline Colour developToTap(const PointwisePlan& plan, const PixelAmounts& at,
                                         Colour colour, Tap tap, const PixelContext& context) {
    switch (tap) {
    case Tap::CurveInput:
        return developToCurveInput(plan, at, colour, context);
    }
    // Unreachable for a valid tap: sample() validates it before any pixel runs.
    // The identity is what an unrecognised tap would sample; ::arraw::tapEncoding
    // and the GPU's probeFor throw for it.
    return colour;
}

/// @brief Applies the pointwise stages up to a tap, with the plan's own amounts.
/// @param plan Resolved settings.
/// @param colour Source colour, in the encoding the plan was built for.
/// @param tap Where to stop.
/// @param context The Presence context at the pixel, as for ::arraw::developPixel.
/// @return The colour at @p tap, still in the working encoding.
[[nodiscard]] inline Colour developToTap(const PointwisePlan& plan, Colour colour, Tap tap,
                                         const PixelContext& context) {
    return developToTap(plan, globalAmountsOf(plan), colour, tap, context);
}

/// @brief Groups the plan's fields that ::arraw::developToCurveInput reads.
///
/// Kept beside the prefix it describes, and changed with it: a field the
/// prefix starts reading must join this list, or ::arraw::sameAtTap would call
/// a stale sample current.
/// @param plan Pointwise block to view.
/// @return White balance and the matrix, exposure, Basic Tone and Presence by reference, and
/// the local fields before the tap by value (::arraw::preTapLocalFieldsOf): the masks that carry
/// one of the eleven controls before the tap and the globals they add to.
[[nodiscard]] inline auto curveInputFieldsOf(const PointwisePlan& plan) {
    return std::tuple<const Matrix3&, const TonePlan&, const PresencePlan&, PreTapLocal>(
        plan.toWorking, plan.tone, plan.presence, preTapLocalFieldsOf(plan.local));
}

} // namespace arraw
