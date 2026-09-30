#version 440

// The pointwise chain: ProcessingPlan.h's developPixel, stage for stage.
//
// Every function below mirrors the C++ of the same name in
// src/core/ProcessingPlan.h, in the same order and with the same arithmetic,
// and must change with it. The Pointwise block is the contract with
// GpuPointwiseBlock in GpuPlan.h: same members, same order.
//
// Powers. C++ std::pow and GLSL pow differ where GLSL leaves the result
// undefined: for x < 0, and for x == 0 with y <= 0. Where the CPU chain can
// reach them:
//   - toPerceptual is only called with luminance > 0, or with exactly 0 by the
//     lifted black; the contrast power takes its result, so x >= 0.
//   - toLinear takes max(value, 0), so x >= 0.
//   - The exponents are 1/2.2, 2.2 and contrastSlope = exp2(c / 200) > 0.
// So the only case that needs care is x == 0, where std::pow gives 0 and a
// driver's exp2(y * log2(x)) gives it only by way of infinities. pow0 spells
// it out. It also returns NaN for x < 0 (as std::pow does for a non-integer
// exponent), although nothing reaches that today, so that a future caller
// cannot silently get an undefined value. NaN and +inf pass through pow as on
// the CPU: a NaN fails every comparison below the way it does there.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D source;

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
} plan;

// 1 / 2.2f and 2.2f as C++ rounds them to float, spelled out so that no
// compiler folds the division at another precision.
const float perceptualExponent = 0.454545438;
const float linearExponent = 2.20000005;


// greyPivot, ProcessingPlan.h. The shader does not use it: the plan carries
// contrastScale, which is derived from it. It is kept so that the mirrored
// constants stay in one place if a stage ever needs it.
const float greyPivot = 0.45865646;

// colorspaces::workingLuminance, ColorSpaces.h.
const vec3 workingLuminance = vec3(0.2627, 0.6780, 0.0593);

// std::pow for the arguments the chain gives it; see the note at the top.
float pow0(float x, float y) {
    if (x > 0.0) {
        return pow(x, y);
    }
    if (x == 0.0) {
        return 0.0;
    }
    // Negative, or NaN: NaN, as std::pow with a non-integer exponent.
    return uintBitsToFloat(0x7fc00000u);
}

// std::clamp(value, low, high), which the built-in clamp is not: it leaves a
// NaN alone where the built-in is undefined.
float clampExact(float value, float low, float high) {
    return value < low ? low : (high < value ? high : value);
}

// smoothstep, ProcessingPlan.h. Not the built-in, which is undefined for
// first >= last and leaves the clamping to the driver.
float smoothStep(float first, float last, float value) {
    const float t = clampExact((value - first) / (last - first), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

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

float shapeLuminance(float luminance) {
    float value = toPerceptual(luminance);
    value = plan.contrastScale * pow0(value, plan.contrastSlope);

    value += plan.shadowShift * shadowWeight(value);
    value += plan.highlightShift * highlightWeight(value);
    value += plan.blackShift * blackWeight(value);
    value += plan.whiteShift * whiteWeight(value);
    // std::max(value, 0.0F): a NaN stays a NaN.
    return toLinear(value < 0.0 ? 0.0 : value);
}

float luminanceOf(vec3 colour) {
    return workingLuminance.x * colour.x + workingLuminance.y * colour.y +
           workingLuminance.z * colour.z;
}

vec3 shapeTone(vec3 colour) {
    if (plan.shapesTone == 0u) {
        return colour;
    }

    const float luminance = luminanceOf(colour);
    if (!(luminance > 0.0)) {
        const float lifted = shapeLuminance(0.0);
        return vec3(lifted, lifted, lifted);
    }

    const float ratio = shapeLuminance(luminance) / luminance;
    return vec3(colour.x * ratio, colour.y * ratio, colour.z * ratio);
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

// PointwiseProbe values, GpuPlan.h.
const uint probeAfterMatrix = 1u;
const uint probeAfterExposure = 2u;
const uint probeAfterTone = 3u;

void main() {
    const vec4 texel = texelFetch(source, ivec2(gl_FragCoord.xy), 0);

    // developPixel, with a stop after the stage a probe names. Alpha is not
    // developed; it goes through as it came.
    vec3 colour = texel.rgb;
    colour = vec3(plan.toWorking[0].x * colour.x + plan.toWorking[0].y * colour.y +
                      plan.toWorking[0].z * colour.z,
                  plan.toWorking[1].x * colour.x + plan.toWorking[1].y * colour.y +
                      plan.toWorking[1].z * colour.z,
                  plan.toWorking[2].x * colour.x + plan.toWorking[2].y * colour.y +
                      plan.toWorking[2].z * colour.z);
    if (plan.probe == probeAfterMatrix) {
        fragColor = vec4(colour, texel.a);
        return;
    }

    colour = vec3(colour.x * plan.exposureGain, colour.y * plan.exposureGain,
                  colour.z * plan.exposureGain);
    if (plan.probe == probeAfterExposure) {
        fragColor = vec4(colour, texel.a);
        return;
    }

    colour = shapeTone(colour);
    if (plan.probe == probeAfterTone) {
        fragColor = vec4(colour, texel.a);
        return;
    }

    fragColor = vec4(rollHighlights(colour), texel.a);
}
