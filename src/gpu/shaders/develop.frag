#version 440
#extension GL_GOOGLE_include_directive : require

// The pointwise chain: PointwisePlan.h's developPixel, stage for stage.
//
// Every function below mirrors the C++ of the same name in
// src/core/PointwisePlan.h and src/core/TonePlan.h (and, for the colour
// grade, src/core/ColorGrading.h and ColorGrading.cpp), in the same order and
// with the same arithmetic, and must change with it. The Pointwise block is
// the contract with GpuPointwiseBlock in GpuPlan.h: same members, same order.
// The structs TonePlan, PresenceAmounts, ChromaAmounts and PixelAmounts below
// are locals that mirror the C++ structs of the same name (TonePlan.h,
// Presence.h, ColorAdjustments.h, PointwisePlan.h); the block does not hold
// them, globalAmounts() builds them from it.
//
// Powers. C++ std::pow and GLSL pow differ where GLSL leaves the result
// undefined: for x < 0, and for x == 0 with y <= 0. Where the CPU chain can
// reach them:
//   - toPerceptual is called with luminance > 0, with exactly 0 (the lifted
//     black), with a tone curve anchor, or with the grade's clamp(Y, 0, 1),
//     which can be 0 or NaN; the argument is never below 0, and the contrast
//     power takes its result, so x >= 0.
//   - toLinear takes max(value, 0), so x >= 0.
//   - The exponents are 1/2.2, 2.2 and contrastSlope = exp2(c / 200) > 0.
// So the only case that needs care is x == 0, where std::pow gives 0 and a
// driver's exp2(y * log2(x)) gives it only by way of infinities. pow0
// (common/exact.glsl) spells it out. NaN and +inf pass through pow as on the
// CPU: a NaN fails every comparison below the way it does there.
//
// Parity with the CPU, including NaN and infinity, and the tolerances the
// tests hold, are measured on Vulkan (lavapipe) only. HLSL and MSL compilers
// may apply fast-math to comparisons and arithmetic; that is unverified.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D source;

// The tone curves' tables, ::arraw::packToneCurves: texel i holds entry i of
// the luma (r), red (g), green (b) and blue (a) curves. Read with texelFetch
// and blended by hand, never through the sampler's filtering, so that the
// arithmetic is the CPU's. Bound even when no curve is active; not read then.
layout(binding = 2) uniform sampler2D curves;

// The Presence context's grids, ::arraw::PresenceContext: log2 luminance in r
// on a box-reduced grid, Texture's base (fine), Clarity's base, the coarse
// cells unblurred (which Clarity and a positive Dehaze read), and Dehaze's
// floor (read where Dehaze is positive) and mean (where it is negative) on the
// same cells. Read with texelFetch and blended by hand, as the CPU's
// PresenceSampler does. Bound even when Presence is off, and a base the
// block's local flags say does not exist is bound to the source and never read.
layout(binding = 3) uniform sampler2D fineBase;
layout(binding = 4) uniform sampler2D coarseBase;
layout(binding = 5) uniform sampler2D coarseCells;
layout(binding = 6) uniform sampler2D hazeFloor;
layout(binding = 7) uniform sampler2D hazeMean;

// LocalMask, GpuLocalMask in GpuPlan.h and LocalMaskPlan in LocalPlan.h: one
// mask resolved against the size of the source being rendered (ADR 044).
//   header  kind, invert, coverage texture, coverage channel (the last two for brush masks)
//   shapeA  linear: alpha, beta, gamma, 0.  radial: centreX, centreY, inner, 0
//   shapeB  radial: the matrix, row-major.  linear: 0
//   k       opacity * delta of each local control, in the table's order
struct LocalMask {
    uvec4 header;
    vec4 shapeA;
    vec4 shapeB;
    vec4 k[4];
};

layout(std140, binding = 1) uniform Pointwise {
    vec4 toWorking[3];
    float exposureGain;
    float contrastSlope;
    float contrastScale;
    float shadowShift;
    float highlightShift;
    float blackShift;
    float whiteShift;
    float shoulderKnee;
    uint shapesTone;
    uint rollsHighlights;
    uint probe;
    uint convertsToGrayscale;
    float saturation;
    float vibrance;
    uint adjustsSaturation;
    uint adjustsVibrance;
    uint adjustsHsl;
    uint curvesLuma;
    uint curvesRed;
    uint curvesGreen;
    uint curvesBlue;
    // Three words of padding, which std140 does not need declared: the band
    // sets below start on the next vec4 boundary by themselves.
    vec4 hueShift[2];
    vec4 bandSaturation[2];
    vec4 bandLuminance[2];
    vec4 grayMix[2];
    uint grades;
    float gradeBalanceShift;
    float gradeZoneWidth;
    // One word of padding, declared so that the vec4s below are plainly
    // where GpuPointwiseBlock puts them.
    uint gradePadding;
    vec4 gradeShadowMidtoneTint;
    vec4 gradeHighlightTint;
    vec4 presenceLumaRow;
    uint presence;
    float textureAmount;
    float clarityAmount;
    float dehazeAmount;
    uint fineReduction;
    uint coarseReduction;
    uvec2 fineGridSize;
    uvec2 coarseGridSize;
    uvec2 presencePadding;
    // The local adjustments (ADR 044): count, touched bits, flags (localFlag...) and padding;
    // the photograph's own value of each local control; and the masks.
    uvec4 localHeader;
    vec4 localGlobal[4];
    LocalMask local[16];
} plan;

#include "common/perceptual.glsl"


// greyPivot, TonePlan.h. The shader does not use it: the plan carries
// contrastScale, which is derived from it. It is kept so that the mirrored
// constants stay in one place if a stage ever needs it.
const float greyPivot = 0.45865646;

// colorspaces::workingLuminance, ColorSpaces.h.
const vec3 workingLuminance = vec3(0.2627, 0.6780, 0.0593);

// liftedBlackThreshold, TonePlan.h: luminance at or below which a colour
// is black for tone. Not zero, because GPUs flush denormals and the CPU does
// not, so a denormal luminance would branch differently on the two.
const float liftedBlackThreshold = 1.0e-20;

// pow0, clampExact and smoothStep; see the note at the top.
#include "common/exact.glsl"

float toPerceptual(float luminance) {
    return pow0(luminance, perceptualExponent);
}

float toLinear(float value) {
    return pow0(value, linearExponent);
}

float shadowWeight(float value) {
    return smoothStep(0.0, 0.3, value) * (1.0 - smoothStep(0.3, 0.6, value));
}

float highlightWeight(float value) {
    return smoothStep(0.4, 0.75, value) * (1.0 - smoothStep(0.75, 1.2, value));
}

float blackWeight(float value) {
    return 1.0 - smoothStep(0.0, 0.35, value);
}

float whiteWeight(float value) {
    return smoothStep(0.6, 1.0, value);
}

// TonePlan, TonePlan.h: Exposure and Basic Tone, resolved.
struct TonePlan {
    float exposureGain;
    bool shapesTone;
    float contrastSlope;
    float contrastScale;
    float shadowShift;
    float highlightShift;
    float blackShift;
    float whiteShift;
};

// PresenceAmounts, Presence.h.
struct PresenceAmounts {
    float texture;
    float clarity;
    float dehaze;
};

// ChromaAmounts, ColorAdjustments.h.
struct ChromaAmounts {
    bool adjustsSaturation;
    float saturation;
    bool adjustsVibrance;
    float vibrance;
};

// PixelAmounts, PointwisePlan.h: the values a pixel applies as its own.
struct PixelAmounts {
    bool balances;
    vec3 balance;
    TonePlan tone;
    PresenceAmounts presence;
    ChromaAmounts chroma;
};

// globalAmountsOf, PointwisePlan.h: every pixel has the photograph's amounts.
PixelAmounts globalAmounts() {
    return PixelAmounts(
        false, vec3(1.0, 1.0, 1.0),
        TonePlan(plan.exposureGain, plan.shapesTone != 0u, plan.contrastSlope, plan.contrastScale,
                 plan.shadowShift, plan.highlightShift, plan.blackShift, plan.whiteShift),
        PresenceAmounts(plan.textureAmount, plan.clarityAmount, plan.dehazeAmount),
        ChromaAmounts(plan.adjustsSaturation != 0u, plan.saturation, plan.adjustsVibrance != 0u,
                      plan.vibrance));
}

// The local adjustments (ADR 044): LocalPlan.h, and amountsAt in PointwisePlan.h.

// GpuLocalFlag, GpuPlan.h: which of the Presence context's bases exist.
const uint localFlagFineBase = 1u;
const uint localFlagCoarseBase = 2u;
const uint localFlagHazeFloor = 4u;
const uint localFlagHazeMean = 8u;

bool hasBase(uint flag) {
    return (plan.localHeader.z & flag) != 0u;
}

// LocalMaskKind, LocalPlan.h.
const uint maskKindLinear = 0u;
const uint maskKindRadial = 1u;

// The rows of the local table, SettingDescriptors.h and LocalControl in LocalPlan.h.
const uint controlTemperature = 0u;
const uint controlTint = 1u;
const uint controlExposure = 2u;
const uint controlContrast = 3u;
const uint controlHighlights = 4u;
const uint controlShadows = 5u;
const uint controlWhites = 6u;
const uint controlBlacks = 7u;
const uint controlTexture = 8u;
const uint controlClarity = 9u;
const uint controlDehaze = 10u;
const uint controlSaturation = 11u;
const uint controlVibrance = 12u;
const uint controlCount = 13u;

// The global controls' ranges, ToneSettings.h, PresenceSettings.h and ColorSettings.h; the
// local ones' clamp for Temperature and Tint, localControlLimit in SettingDescriptors.h.
const float darkestExposure = -5.0;
const float brightestExposure = 5.0;
const float flattestContrast = -100.0;
const float steepestContrast = 100.0;
const float weakestToneControl = -100.0;
const float strongestToneControl = 100.0;
const float weakestPresence = -100.0;
const float strongestPresence = 100.0;
const float weakestSaturation = -100.0;
const float strongestSaturation = 100.0;
const float localControlLimit = 100.0;

// regionalReach and endpointReach, TonePlan.h.
const float regionalReach = 0.12;
const float endpointReach = 0.08;

// temperatureRedStops, temperatureBlueStops, tintRedStops and tintBlueStops, LocalPlan.h.
const float temperatureRedStops = 0.16;
const float temperatureBlueStops = -0.43;
const float tintRedStops = 0.19;
const float tintBlueStops = 0.31;

// maskWeight, LocalPlan.h: the weight of a mask at a pixel centre, 0 to 1.
float maskWeight(LocalMask mask, float x, float y) {
    float weight = 0.0;
    if (mask.header.x == maskKindLinear) {
        const float t = mask.shapeA.x * x + mask.shapeA.y * y + mask.shapeA.z;
        weight = 1.0 - smoothStep(0.0, 1.0, t);
    } else {
        const float dx = x - mask.shapeA.x;
        const float dy = y - mask.shapeA.y;
        const float qx = mask.shapeB.x * dx + mask.shapeB.y * dy;
        const float qy = mask.shapeB.z * dx + mask.shapeB.w * dy;
        const float d = sqrt(qx * qx + qy * qy);
        weight = 1.0 - smoothStep(mask.shapeA.z, 1.0, d);
    }
    return mask.header.y != 0u ? 1.0 - weight : weight;
}

// relativeBalanceGainFor, LocalPlan.h: relative Temperature and Tint as a gain per working
// channel, normalised by the working luminance row.
vec3 relativeBalanceGainFor(float temperature, float tint) {
    const float t = temperature / localControlLimit;
    const float n = tint / localControlLimit;
    const float red = exp2(temperatureRedStops * t + tintRedStops * n);
    const float blue = exp2(temperatureBlueStops * t + tintBlueStops * n);
    const float norm = workingLuminance.x * red + workingLuminance.y + workingLuminance.z * blue;
    return vec3(red / norm, 1.0 / norm, blue / norm);
}

// amountsAt, PointwisePlan.h: for each control the sum of the masks' weighted amounts, in list
// order and in float (localSumsAt, LocalPlan.h); where the sum is exactly zero the pixel takes
// the photograph's resolved value, otherwise the global value plus the sum, clamped once, is
// resolved by the expression the global setting uses.
PixelAmounts amountsAt(ivec2 at) {
    PixelAmounts amounts = globalAmounts();
    // The centre of the pixel, in source pixels.
    const float x = float(at.x) + 0.5;
    const float y = float(at.y) + 0.5;

    float sums[13];
    for (uint control = 0u; control < controlCount; ++control) {
        sums[control] = 0.0;
    }
    for (uint index = 0u; index < plan.localHeader.x; ++index) {
        const float weight = maskWeight(plan.local[index], x, y);
        for (uint control = 0u; control < controlCount; ++control) {
            if (((plan.localHeader.y >> control) & 1u) != 0u) {
                // Not fused with the sum: two masks that cancel exactly must cancel to zero.
                precise float term = weight * plan.local[index].k[control >> 2][control & 3u];
                sums[control] += term;
            }
        }
    }

    const float temperature = sums[controlTemperature];
    const float tint = sums[controlTint];
    if (temperature != 0.0 || tint != 0.0) {
        amounts.balances = true;
        amounts.balance =
            relativeBalanceGainFor(clampExact(temperature, -localControlLimit, localControlLimit),
                                   clampExact(tint, -localControlLimit, localControlLimit));
    }

    // The global value and the pixel's sum, clamped once to the global control's range.
#define EFFECTIVE(control, least, most)                                                            \
    clampExact(plan.localGlobal[(control) >> 2][(control) & 3u] + sums[control], least, most)

    if (sums[controlExposure] != 0.0) {
        amounts.tone.exposureGain =
            exp2(EFFECTIVE(controlExposure, darkestExposure, brightestExposure));
    }
    if (sums[controlContrast] != 0.0) {
        amounts.tone.contrastSlope =
            exp2(EFFECTIVE(controlContrast, flattestContrast, steepestContrast) /
                 (2.0 * steepestContrast));
        amounts.tone.contrastScale = pow0(greyPivot, 1.0 - amounts.tone.contrastSlope);
        amounts.tone.shapesTone = true;
    }
    if (sums[controlHighlights] != 0.0) {
        amounts.tone.highlightShift =
            regionalReach * EFFECTIVE(controlHighlights, weakestToneControl, strongestToneControl) /
            strongestToneControl;
        amounts.tone.shapesTone = true;
    }
    if (sums[controlShadows] != 0.0) {
        amounts.tone.shadowShift =
            regionalReach * EFFECTIVE(controlShadows, weakestToneControl, strongestToneControl) /
            strongestToneControl;
        amounts.tone.shapesTone = true;
    }
    if (sums[controlWhites] != 0.0) {
        amounts.tone.whiteShift =
            endpointReach * EFFECTIVE(controlWhites, weakestToneControl, strongestToneControl) /
            strongestToneControl;
        amounts.tone.shapesTone = true;
    }
    if (sums[controlBlacks] != 0.0) {
        amounts.tone.blackShift =
            endpointReach * EFFECTIVE(controlBlacks, weakestToneControl, strongestToneControl) /
            strongestToneControl;
        amounts.tone.shapesTone = true;
    }

    if (sums[controlTexture] != 0.0) {
        amounts.presence.texture =
            EFFECTIVE(controlTexture, weakestPresence, strongestPresence) / strongestPresence;
    }
    if (sums[controlClarity] != 0.0) {
        amounts.presence.clarity =
            EFFECTIVE(controlClarity, weakestPresence, strongestPresence) / strongestPresence;
    }
    if (sums[controlDehaze] != 0.0) {
        amounts.presence.dehaze =
            EFFECTIVE(controlDehaze, weakestPresence, strongestPresence) / strongestPresence;
    }

    if (sums[controlSaturation] != 0.0 || sums[controlVibrance] != 0.0) {
        // chromaAmountsFor, ColorAdjustments.h.
        const float saturation =
            EFFECTIVE(controlSaturation, weakestSaturation, strongestSaturation);
        const float vibrance = EFFECTIVE(controlVibrance, weakestSaturation, strongestSaturation);
        amounts.chroma = ChromaAmounts(saturation != 0.0, saturation / strongestSaturation,
                                       vibrance != 0.0, vibrance / strongestSaturation);
    }
#undef EFFECTIVE
    return amounts;
}

float shapeLuminance(TonePlan tone, float luminance) {
    float value = toPerceptual(luminance);
    value = tone.contrastScale * pow0(value, tone.contrastSlope);

    value += tone.shadowShift * shadowWeight(value);
    value += tone.highlightShift * highlightWeight(value);
    value += tone.blackShift * blackWeight(value);
    value += tone.whiteShift * whiteWeight(value);
    // std::max(value, 0.0F): a NaN stays a NaN.
    return toLinear(value < 0.0 ? 0.0 : value);
}

float luminanceOf(vec3 colour) {
    return workingLuminance.x * colour.x + workingLuminance.y * colour.y +
           workingLuminance.z * colour.z;
}

vec3 shapeTone(TonePlan tone, vec3 colour) {
    if (!tone.shapesTone) {
        return colour;
    }

    const float luminance = luminanceOf(colour);
    if (!(luminance > liftedBlackThreshold)) {
        const float lifted = shapeLuminance(tone, 0.0);
        return vec3(lifted, lifted, lifted);
    }

    const float ratio = shapeLuminance(tone, luminance) / luminance;
    return vec3(colour.x * ratio, colour.y * ratio, colour.z * ratio);
}

// toneCurveSamples, ToneCurve.h.
const int toneCurveSamples = 1024;

// One entry of one curve's table: channel 0 is luma, 1 red, 2 green, 3 blue.
float curveEntry(int index, int channel) {
    const vec4 texel = texelFetch(curves, ivec2(index, 0), 0);
    return channel == 0 ? texel.x : (channel == 1 ? texel.y : (channel == 2 ? texel.z : texel.w));
}

// evaluateCurve, ToneCurve.h: the same index and weight, the same extension
// at slope one above one, and the value at zero for anything not above it.
float evaluateCurve(int channel, float x) {
    const int last = toneCurveSamples - 1;
    if (!(x > 0.0)) {
        return curveEntry(0, channel);
    }
    if (x >= 1.0) {
        return curveEntry(last, channel) + (x - 1.0);
    }
    const float position = x * float(last);
    const int index = min(int(position), last - 1);
    const float fraction = position - float(index);
    const float low = curveEntry(index, channel);
    const float high = curveEntry(index + 1, channel);
    return low + fraction * (high - low);
}

// curveRatioFloor, PointwisePlan.h: luminance below which the luma curve's
// ratio is held, 2^-14.
const float curveRatioFloor = 6.103515625e-05;

// applyToneCurves' channel curve, PointwisePlan.h: a negative channel moves
// by the lift, a NaN or zero one takes it.
float curveChannel(int channel, float value) {
    if (value > 0.0) {
        const float shaped = evaluateCurve(channel, toPerceptual(value));
        return toLinear(shaped < 0.0 ? 0.0 : shaped);
    }
    const float black = evaluateCurve(channel, 0.0);
    const float lift = toLinear(black < 0.0 ? 0.0 : black);
    return value < 0.0 ? value + lift : lift;
}

// applyToneCurves, PointwisePlan.h: luminance first, then each channel.
vec3 applyToneCurves(vec3 colour) {
    if (plan.curvesLuma != 0u) {
        const float black = evaluateCurve(0, 0.0);
        const float lift = toLinear(black < 0.0 ? 0.0 : black);
        const float luminance = luminanceOf(colour);
        // A NaN fails both comparisons.
        if (!(luminance > curveRatioFloor) && !(luminance <= curveRatioFloor)) {
            colour = vec3(lift, lift, lift);
        } else {
            const float anchor = luminance > curveRatioFloor ? luminance : curveRatioFloor;
            const float value = evaluateCurve(0, toPerceptual(anchor));
            const float shaped = toLinear(value < 0.0 ? 0.0 : value);
            const float ratio = (shaped - lift) / anchor;
            colour = vec3(colour.x * ratio + lift, colour.y * ratio + lift,
                          colour.z * ratio + lift);
        }
    }
    if (plan.curvesRed != 0u) {
        colour.x = curveChannel(1, colour.x);
    }
    if (plan.curvesGreen != 0u) {
        colour.y = curveChannel(2, colour.y);
    }
    if (plan.curvesBlue != 0u) {
        colour.z = curveChannel(3, colour.z);
    }
    return colour;
}

vec3 rollHighlights(vec3 colour) {
    // No shoulder: the CPU compares against an infinite knee, which nothing
    // exceeds; the block carries a flag rather than the infinity.
    if (plan.rollsHighlights == 0u) {
        return colour;
    }

    const float luminance = luminanceOf(colour);
    if (!(luminance > plan.shoulderKnee)) {
        return colour;
    }

    const float headroom = 1.0 - plan.shoulderKnee;
    const float above = (luminance - plan.shoulderKnee) / headroom;
    const float rolled = plan.shoulderKnee + headroom * (above / (1.0 + above));

    const float ratio = rolled / luminance;
    const float chroma = ratio * sqrt(ratio);
    return vec3(rolled + chroma * (colour.x * ratio - rolled),
                rolled + chroma * (colour.y * ratio - rolled),
                rolled + chroma * (colour.z * ratio - rolled));
}

// ColorAdjustments.h and ColorAdjustments.cpp, stage for stage. The colour
// block follows the shoulder (ADR 027).

// std::max and std::min, which the built-ins are not: they return their first
// argument when the comparison fails, as the built-ins leave undefined.
float maxExact(float a, float b) {
    return a < b ? b : a;
}

float minExact(float a, float b) {
    return b < a ? b : a;
}

// std::cbrt, for the arguments Oklab gives it: negative channels keep their sign.
float cbrtExact(float x) {
    const float third = 0.333333343;
    return x < 0.0 ? -pow0(-x, third) : pow0(x, third);
}

// hslCenters, ColorAdjustments.cpp.
const float hslCenters[8] = float[8](0.0, 0.083, 0.167, 0.333, 0.5, 0.611, 0.778, 0.889);

// vibranceHalf and negligibleWeight, ColorAdjustments.cpp.
const float vibranceHalf = 0.2;
const float negligibleWeight = 0.001;

// toHsv: (hue, saturation, value).
vec3 toHsv(vec3 c) {
    const float maxC = maxExact(c.x, maxExact(c.y, c.z));
    const float minC = minExact(c.x, minExact(c.y, c.z));
    const float delta = maxC - minC;
    float h = 0.0;
    if (delta > 1e-5) {
        if (maxC == c.x) {
            h = (c.y - c.z) / delta + (c.y < c.z ? 6.0 : 0.0);
        } else if (maxC == c.y) {
            h = (c.z - c.x) / delta + 2.0;
        } else {
            h = (c.x - c.y) / delta + 4.0;
        }
        h /= 6.0;
    }
    const float s = (maxC > 1e-5) ? delta / maxC : 0.0;
    return vec3(h, s, maxC);
}

vec3 fromHsv(vec3 hsv) {
    const float h = hsv.x * 6.0;
    const float s = hsv.y;
    const float v = hsv.z;
    const float whole = floor(h);
    const int i = (whole >= 0.0 && whole < 6.0) ? int(whole) : 5;
    const float f = h - float(i);
    const float p = v * (1.0 - s);
    const float q = v * (1.0 - s * f);
    const float t = v * (1.0 - s * (1.0 - f));
    if (i == 0) return vec3(v, t, p);
    if (i == 1) return vec3(q, v, p);
    if (i == 2) return vec3(p, v, t);
    if (i == 3) return vec3(p, q, v);
    if (i == 4) return vec3(t, p, v);
    return vec3(v, p, q);
}

float bandWeight(float hue, int band) {
    float d = abs(hue - hslCenters[band]);
    if (d > 0.5) {
        d = 1.0 - d;
    }
    const float w = maxExact(0.0, 1.0 - d * 6.0);
    return w * w * (3.0 - 2.0 * w);
}

// toOklab, as (lightness, a, b).
vec3 toOklab(vec3 rgb) {
    const float r = 1.660491 * rgb.x - 0.587641 * rgb.y - 0.072850 * rgb.z;
    const float g = -0.124550 * rgb.x + 1.132900 * rgb.y - 0.008349 * rgb.z;
    const float bl = -0.018151 * rgb.x - 0.100579 * rgb.y + 1.118730 * rgb.z;
    const float l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * bl;
    const float m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * bl;
    const float s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * bl;
    const float l_ = cbrtExact(l);
    const float m_ = cbrtExact(m);
    const float s_ = cbrtExact(s);
    return vec3(0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
                1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
                0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_);
}

vec3 fromOklab(vec3 lab) {
    const float l_ = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    const float m_ = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    const float s_ = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;
    const float r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    const float g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    const float b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    return vec3(0.627404 * r + 0.329283 * g + 0.043313 * b,
                0.069097 * r + 0.919541 * g + 0.011362 * b,
                0.016391 * r + 0.088013 * g + 0.895595 * b);
}

vec3 applySaturation(vec3 colour, float amount) {
    vec3 lab = toOklab(colour);
    const float scale = 1.0 + amount;
    lab.y *= scale;
    lab.z *= scale;
    return fromOklab(lab);
}

// Presence.h and Presence.cpp: Texture, Clarity and Dehaze (ADR 041).

// presenceLuminanceFloor, presenceLuminanceCeiling and boundedLuminance.
#include "common/presence_bounds.glsl"

// textureLimitStops, clarityLimitStops, dehazeStrength, dehazeVeil,
// dehazeChroma and dehazeMeanLimitStops, Presence.h.
const float textureLimitStops = 0.5;
const float clarityLimitStops = 1.0;
const float dehazeStrength = 0.6;
const float dehazeVeil = 0.4;
const float dehazeChroma = 0.16;
const float dehazeMeanLimitStops = 6.0;

float presenceLogLuminance(vec3 colour) {
    const float luminance = plan.presenceLumaRow.x * colour.x + plan.presenceLumaRow.y * colour.y +
                            plan.presenceLumaRow.z * colour.z;
    return log2(boundedLuminance(luminance));
}

float softLimit(float detail, float limit) {
    return detail / (1.0 + abs(detail) / limit);
}

float midtoneWeight(float value) {
    return smoothStep(0.0, 0.8, 1.0 - abs(2.0 * value - 1.0));
}

// gridTap, ReducedGrid.h: u = (x + 0.5) / reduction - 0.5, clamped; exact in
// float, the reduction being a power of two.
void gridTap(int coordinate, uint reduction, uint cells, out int first, out int second,
             out float fraction) {
    const float position = (float(coordinate) + 0.5) / float(reduction) - 0.5;
    const float clamped = clampExact(position, 0.0, float(cells - 1u));
    const float below = floor(clamped);
    first = int(below);
    second = min(first + 1, int(cells) - 1);
    fraction = clamped - below;
}

// upsampled, Presence.cpp: the grid read bilinearly, rows first.
float upsampledFine(ivec2 at) {
    int x0;
    int x1;
    float tx;
    int y0;
    int y1;
    float ty;
    gridTap(at.x, plan.fineReduction, plan.fineGridSize.x, x0, x1, tx);
    gridTap(at.y, plan.fineReduction, plan.fineGridSize.y, y0, y1, ty);
    const float topLeft = texelFetch(fineBase, ivec2(x0, y0), 0).r;
    const float topRight = texelFetch(fineBase, ivec2(x1, y0), 0).r;
    const float bottomLeft = texelFetch(fineBase, ivec2(x0, y1), 0).r;
    const float bottomRight = texelFetch(fineBase, ivec2(x1, y1), 0).r;
    const float top = topLeft + tx * (topRight - topLeft);
    const float bottom = bottomLeft + tx * (bottomRight - bottomLeft);
    return top + ty * (bottom - top);
}

// The coarse grids share one geometry; GLSL takes no sampler parameter here
// portably, so the four reads are spelled out by which.
const int coarseOfBase = 0;
const int coarseOfCells = 1;
const int coarseOfHazeFloor = 2;
const int coarseOfHazeMean = 3;

float coarseAt(int which, ivec2 at) {
    if (which == coarseOfBase) {
        return texelFetch(coarseBase, at, 0).r;
    }
    if (which == coarseOfCells) {
        return texelFetch(coarseCells, at, 0).r;
    }
    if (which == coarseOfHazeFloor) {
        return texelFetch(hazeFloor, at, 0).r;
    }
    return texelFetch(hazeMean, at, 0).r;
}

float upsampledCoarse(int which, ivec2 at) {
    int x0;
    int x1;
    float tx;
    int y0;
    int y1;
    float ty;
    gridTap(at.x, plan.coarseReduction, plan.coarseGridSize.x, x0, x1, tx);
    gridTap(at.y, plan.coarseReduction, plan.coarseGridSize.y, y0, y1, ty);
    const float topLeft = coarseAt(which, ivec2(x0, y0));
    const float topRight = coarseAt(which, ivec2(x1, y0));
    const float bottomLeft = coarseAt(which, ivec2(x0, y1));
    const float bottomRight = coarseAt(which, ivec2(x1, y1));
    const float top = topLeft + tx * (topRight - topLeft);
    const float bottom = bottomLeft + tx * (bottomRight - bottomLeft);
    return top + ty * (bottom - top);
}

// applyPresence, Presence.cpp. A control acts where its amount at the pixel is not zero and its
// base exists (ADR 044, section 5): a base that does not exist is never read, whatever the
// amount rounded to. A positive Dehaze reads the floor, a negative one the mean, and exactly
// zero neither.
vec3 applyPresence(PresenceAmounts amounts, vec3 colour, float logLuminance, ivec2 at) {
    if (plan.presence == 0u) {
        return colour;
    }
    const float luminance = luminanceOf(colour);
    if (!(luminance > liftedBlackThreshold)) {
        return colour;
    }

    float stops = 0.0;
    if (hasBase(localFlagFineBase) && amounts.texture != 0.0) {
        stops += amounts.texture * softLimit(logLuminance - upsampledFine(at), textureLimitStops);
    }
    if (hasBase(localFlagCoarseBase) && amounts.clarity != 0.0) {
        const float limited =
            softLimit(upsampledCoarse(coarseOfCells, at) - upsampledCoarse(coarseOfBase, at),
                      clarityLimitStops);
        const float perceptual = toPerceptual(clampExact(luminance, 0.0, 1.0));
        stops += amounts.clarity * midtoneWeight(perceptual) * limited;
    }
    const float gain = exp2(stops);
    colour = vec3(colour.x * gain, colour.y * gain, colour.z * gain);

    const bool floorOn = amounts.dehaze > 0.0 && hasBase(localFlagHazeFloor);
    const bool meanOn = amounts.dehaze < 0.0 && hasBase(localFlagHazeMean);
    if (!floorOn && !meanOn) {
        return colour;
    }

    const float toned = luminance * gain;
    const float open = 1.0 - smoothStep(0.75, 1.25, toned);
    float hazy = 0.0;
    if (floorOn) {
        hazy = exp2(min(upsampledCoarse(coarseOfHazeFloor, at) - upsampledCoarse(coarseOfCells, at),
                        0.0)) *
               open;
        const float keep = 1.0 - amounts.dehaze * dehazeStrength * hazy;
        colour = vec3(colour.x * keep, colour.y * keep, colour.z * keep);
    } else {
        const float mean =
            toned * exp2(min(upsampledCoarse(coarseOfHazeMean, at) - logLuminance,
                             dehazeMeanLimitStops));
        const float veil = open > 0.0 ? -amounts.dehaze * dehazeVeil * open * mean : 0.0;
        hazy = veil > 0.0 ? veil / (toned + veil) : 0.0;
        colour = vec3(colour.x + veil, colour.y + veil, colour.z + veil);
    }
    const float chroma = amounts.dehaze * dehazeChroma * hazy;
    return chroma == 0.0 ? colour : applySaturation(colour, chroma);
}

vec3 applyVibrance(vec3 colour, float amount) {
    vec3 lab = toOklab(colour);
    const float chroma = sqrt(lab.y * lab.y + lab.z * lab.z);
    const float weight = vibranceHalf / (vibranceHalf + chroma);
    const float scale = 1.0 + amount * weight;
    lab.y *= scale;
    lab.z *= scale;
    return fromOklab(lab);
}

vec3 applyHsl(vec3 colour) {
    vec3 hsv = toHsv(colour);

    float totalHue = 0.0;
    float totalSat = 0.0;
    float totalLum = 0.0;
    float totalW = 0.0;
    for (int i = 0; i < 8; ++i) {
        const float w = bandWeight(hsv.x, i);
        if (w > negligibleWeight) {
            totalHue += plan.hueShift[i >> 2][i & 3] * w;
            totalSat += plan.bandSaturation[i >> 2][i & 3] * w;
            totalLum += plan.bandLuminance[i >> 2][i & 3] * w;
            totalW += w;
        }
    }
    if (totalW < negligibleWeight) {
        return colour;
    }

    const float wInv = 1.0 / totalW;
    const float turned = hsv.x + totalHue * wInv / 12.0;
    hsv.x = turned - floor(turned);
    hsv.y = clampExact(hsv.y * (1.0 + totalSat * wInv * 0.5), 0.0, 1.0);
    hsv.z = maxExact(hsv.z + totalLum * wInv * 0.5, 0.0);
    return fromHsv(hsv);
}

vec3 applyBlackAndWhite(vec3 colour) {
    const float base = luminanceOf(colour);
    const vec3 hsv = toHsv(colour);

    float weighted = 0.0;
    float totalW = 0.0;
    for (int i = 0; i < 8; ++i) {
        const float w = bandWeight(hsv.x, i);
        if (w > negligibleWeight) {
            weighted += plan.grayMix[i >> 2][i & 3] * w;
            totalW += w;
        }
    }

    float gain = 1.0;
    if (totalW > negligibleWeight) {
        const float blended = weighted / totalW;
        gain = 1.0 + (blended / 100.0) * hsv.y;
    }
    const float grey = maxExact(base * gain, 0.0);
    return vec3(grey, grey, grey);
}

// ColorGrading.h and ColorGrading.cpp. The zone tints arrive resolved, so no
// sine or cosine is taken here.

// midtoneCentre, tintFadeStart and tintFadeEnd, ColorGrading.cpp.
const float midtoneCentre = 0.5;
const float tintFadeStart = 0.85;
const float tintFadeEnd = 1.0;

float bell(float position, float centre, float width) {
    const float t = (position - centre) / width;
    return exp(-t * t);
}

// gradeZoneWeights: (shadows, midtones, highlights), summing to one.
vec3 gradeZoneWeights(float luminance) {
    const float held = clampExact(luminance, 0.0, 1.0);
    const float position = clampExact(toPerceptual(held) + plan.gradeBalanceShift, 0.0, 1.0);
    const float shadows = bell(position, 0.0, plan.gradeZoneWidth);
    const float midtones = bell(position, midtoneCentre, plan.gradeZoneWidth);
    const float highlights = bell(position, 1.0, plan.gradeZoneWidth);
    const float total = shadows + midtones + highlights;
    return vec3(shadows / total, midtones / total, highlights / total);
}

float gradeTintFade(float lightness) {
    return 1.0 - smoothStep(tintFadeStart, tintFadeEnd, lightness);
}

vec3 applyColorGrading(vec3 colour) {
    if (plan.grades == 0u) {
        return colour;
    }
    const vec3 weights = gradeZoneWeights(luminanceOf(colour));
    const vec4 lower = plan.gradeShadowMidtoneTint;
    const vec2 upper = plan.gradeHighlightTint.xy;
    vec3 lab = toOklab(colour);
    const float fade = gradeTintFade(lab.x);
    if (fade == 0.0) {
        return colour;
    }
    lab.y += fade * (weights.x * lower.x + weights.y * lower.z + weights.z * upper.x);
    lab.z += fade * (weights.x * lower.y + weights.y * lower.w + weights.z * upper.y);
    return fromOklab(lab);
}

vec3 adjustColor(ChromaAmounts chroma, vec3 colour) {
    if (plan.convertsToGrayscale != 0u) {
        colour = applyBlackAndWhite(colour);
    } else {
        if (plan.adjustsHsl != 0u) {
            colour = applyHsl(colour);
        }
        if (chroma.adjustsSaturation) {
            colour = applySaturation(colour, chroma.saturation);
        }
        if (chroma.adjustsVibrance) {
            colour = applyVibrance(colour, chroma.vibrance);
        }
    }
    return applyColorGrading(colour);
}

// PointwiseProbe values, GpuPlan.h.
const uint probeAfterMatrix = 1u;
const uint probeAfterExposure = 2u;
const uint probeAfterTone = 3u;
const uint probeAfterShoulder = 4u;
const uint probeAfterCurves = 5u;

void main() {
    const ivec2 at = ivec2(gl_FragCoord.xy);
    const vec4 texel = texelFetch(source, at, 0);
    // A photograph without masks takes its own amounts at every pixel; amountsAt is the CPU's
    // for one with.
    const PixelAmounts amounts = plan.localHeader.x != 0u ? amountsAt(at) : globalAmounts();

    // developPixel, with a stop after the stage a probe names. Alpha is not
    // developed; it goes through as it came. Presence measures the source
    // colour, before white balance and exposure.
    vec3 colour = texel.rgb;
    const float logLuminance = plan.presence != 0u ? presenceLogLuminance(colour) : 0.0;
    colour = vec3(plan.toWorking[0].x * colour.x + plan.toWorking[0].y * colour.y +
                      plan.toWorking[0].z * colour.z,
                  plan.toWorking[1].x * colour.x + plan.toWorking[1].y * colour.y +
                      plan.toWorking[1].z * colour.z,
                  plan.toWorking[2].x * colour.x + plan.toWorking[2].y * colour.y +
                      plan.toWorking[2].z * colour.z);
    if (amounts.balances) {
        colour = vec3(colour.x * amounts.balance.x, colour.y * amounts.balance.y,
                      colour.z * amounts.balance.z);
    }
    if (plan.probe == probeAfterMatrix) {
        fragColor = vec4(colour, texel.a);
        return;
    }

    colour = vec3(colour.x * amounts.tone.exposureGain, colour.y * amounts.tone.exposureGain,
                  colour.z * amounts.tone.exposureGain);
    if (plan.probe == probeAfterExposure) {
        fragColor = vec4(colour, texel.a);
        return;
    }

    colour = shapeTone(amounts.tone, colour);
    colour = applyPresence(amounts.presence, colour, logLuminance, at);
    if (plan.probe == probeAfterTone) {
        fragColor = vec4(colour, texel.a);
        return;
    }

    colour = applyToneCurves(colour);
    if (plan.probe == probeAfterCurves) {
        fragColor = vec4(colour, texel.a);
        return;
    }

    colour = rollHighlights(colour);
    if (plan.probe == probeAfterShoulder) {
        fragColor = vec4(colour, texel.a);
        return;
    }

    fragColor = vec4(adjustColor(amounts.chroma, colour), texel.a);
}
