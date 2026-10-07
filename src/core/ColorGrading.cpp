#include "ColorGrading.h"

#include "ColorAdjustments.h"
#include "ColorSpaces.h"
#include "ProcessingPlan.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

using namespace arraw;

namespace {

/// @brief Oklab chroma a zone adds at full saturation and full weight.
///
/// main's strength (ADR 0052 there): a tint, clearly visible on a grey but
/// far from the chroma of a saturated colour. Mirrored through the plan.
constexpr float fullGradeChroma = 0.10F;

/// @brief Width of each zone's bell at Blending zero, in the perceptual coordinate.
constexpr float narrowestZoneWidth = 0.18F;

/// @brief Width of each zone's bell at Blending a hundred.
constexpr float widestZoneWidth = 0.45F;

/// @brief How far Balance at its limit moves the tonal position.
constexpr float fullBalanceShift = 0.25F;

/// @brief Centre of the Midtones zone on the tonal position; Shadows sit at zero, Highlights at
/// one.
///
/// Mirrored by `src/gpu/shaders/develop.frag`.
constexpr float midtoneCentre = 0.5F;

/// @brief Oklab lightness from which a tint starts to fade, so that it has headroom to the
/// shoulder.
///
/// The grade runs after the shoulder, and nothing after it rolls a channel
/// back, so a full tint on a near-white tone would push channels far past
/// white and the export would clip them (ADR 034). Below this lightness the
/// tint is whole. Mirrored by `src/gpu/shaders/develop.frag`.
constexpr float tintFadeStart = 0.85F;

/// @brief Oklab lightness at and above which no tint is left: white stays white.
///
/// Mirrored by `src/gpu/shaders/develop.frag`.
constexpr float tintFadeEnd = 1.0F;

/// @brief Degrees in a full turn of the hue wheel.
constexpr float fullTurn = 360.0F;

/// @brief Checks a hue is finite and wraps it onto the wheel, from zero up to a full turn.
///
/// A hue is an angle, so one past the wheel is wrapped rather than clamped:
/// 400 is 40, -30 is 330.
float wrappedHue(float hue) {
    if (!std::isfinite(hue)) {
        throw std::invalid_argument("A colour grading hue adjustment must be finite");
    }
    float wrapped = std::fmod(hue, fullTurn);
    if (wrapped < 0.0F) {
        wrapped += fullTurn;
    }
    // A tiny negative hue wraps to the full turn itself in float arithmetic.
    return wrapped >= fullTurn ? 0.0F : wrapped;
}

/// @brief Resolves one zone into the offset it adds at full weight.
ZoneTint tintOf(const GradeZone& zone) {
    const float hue = wrappedHue(zone.hue);
    const float saturation =
        clampedSetting(zone.saturation, weakestGrade, strongestGrade, "colour grading saturation");
    const float chroma = saturation / strongestGrade * fullGradeChroma;
    const double angle = static_cast<double>(hue) * std::numbers::pi / 180.0;
    return {static_cast<float>(chroma * std::cos(angle)),
            static_cast<float>(chroma * std::sin(angle))};
}

/// @brief Height of one zone's bell at a tonal position.
float bell(float position, float centre, float width) {
    const float t = (position - centre) / width;
    return std::exp(-t * t);
}

} // namespace

ColorGradingPlan arraw::colorGradingPlanFor(const ColorGradingSettings& settings) {
    const ZoneTint shadows = tintOf(settings.shadows);
    const ZoneTint midtones = tintOf(settings.midtones);
    const ZoneTint highlights = tintOf(settings.highlights);
    const float balance = clampedSetting(settings.balance, -gradeBalanceLimit, gradeBalanceLimit,
                                         "colour grading balance");
    const float blending = clampedSetting(settings.blending, sharpestGradeBlending,
                                          softestGradeBlending, "colour grading blending");

    // Saturation alone decides, read after clamping (so a negative one is
    // zero): a hue with no saturation tints nothing, and Balance and Blending
    // only shape tints that exist. Left at the defaults otherwise, so that
    // the plan, too, ignores them.
    if (shadows == ZoneTint{} && midtones == ZoneTint{} && highlights == ZoneTint{}) {
        return {};
    }

    ColorGradingPlan plan;
    plan.active = true;
    plan.shadowTint = shadows;
    plan.midtoneTint = midtones;
    plan.highlightTint = highlights;
    plan.balanceShift = balance / gradeBalanceLimit * fullBalanceShift;
    plan.zoneWidth = narrowestZoneWidth +
                     (widestZoneWidth - narrowestZoneWidth) * (blending / softestGradeBlending);
    return plan;
}

ZoneWeights arraw::gradeZoneWeights(const ColorGradingPlan& plan, float luminance) {
    const float held = std::clamp(luminance, 0.0F, 1.0F);
    const float position = std::clamp(toPerceptual(held) + plan.balanceShift, 0.0F, 1.0F);
    const float shadows = bell(position, 0.0F, plan.zoneWidth);
    const float midtones = bell(position, midtoneCentre, plan.zoneWidth);
    const float highlights = bell(position, 1.0F, plan.zoneWidth);
    // Never zero: no position is further than a quarter from a centre.
    const float total = shadows + midtones + highlights;
    return {shadows / total, midtones / total, highlights / total};
}

float arraw::gradeTintFade(float lightness) {
    return 1.0F - smoothstep(tintFadeStart, tintFadeEnd, lightness);
}

Colour arraw::applyColorGrading(const ColorGradingPlan& plan, Colour colour) {
    if (!plan.active) {
        return colour;
    }
    const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                            colorspaces::workingLuminance[1] * colour[1] +
                            colorspaces::workingLuminance[2] * colour[2];
    const ZoneWeights weights = gradeZoneWeights(plan, luminance);
    Oklab lab = toOklab(colour);
    const float fade = gradeTintFade(lab.lightness);
    if (fade == 0.0F) {
        // At or above white: no tint, and not even the Oklab round trip's rounding.
        return colour;
    }
    lab.a += fade * (weights.shadows * plan.shadowTint.a + weights.midtones * plan.midtoneTint.a +
                     weights.highlights * plan.highlightTint.a);
    lab.b += fade * (weights.shadows * plan.shadowTint.b + weights.midtones * plan.midtoneTint.b +
                     weights.highlights * plan.highlightTint.b);
    return fromOklab(lab);
}
