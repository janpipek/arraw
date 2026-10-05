#version 440

// One filtering step of the Denoise pass: Denoise.cpp's reduction to the grid,
// its colour blur and its separable bilateral, one step per render.
//
// Every function below mirrors the C++ of the same name in src/core/Denoise.cpp,
// with the same arithmetic in the same order, and must change with it. The
// Denoise block is the contract with GpuDenoiseBlock in GpuPlan.h: same
// members, same order. The spatial weights come from the host, so only the
// edge-stop's exp and the perceptual pow are the device's own.
//
// Parity with the CPU, and the tolerance the tests hold, are measured on
// Vulkan (lavapipe) only.

layout(location = 0) out vec4 fragColor;

// The source, the grid of ratios, or the across result (luminance, perceptual), by step.
layout(binding = 0) uniform sampler2D source;

layout(std140, binding = 1) uniform Denoise {
    uint step;
    uint radius;
    uint gridReduction;
    uint luminance;
    uint color;
    float rangeFactor;
    float luminanceMix;
    float colorMix;
    vec4 lumaRow;
    vec4 neutral;
    uvec2 sourceSize;
    uvec2 gridSize;
    vec4 weights[17];
} plan;

// DenoiseStep in GpuPlan.h.
const uint stepReduce = 0u;
const uint stepBlurAcross = 1u;
const uint stepBlurDown = 2u;
const uint stepBilateralAcross = 3u;
const uint stepBilateralDown = 4u;

// denoiseRatioFloor in Denoise.h: 2^-14.
const float ratioFloor = 6.103515625e-05;

// 1 / 2.2f as C++ rounds it to float; see develop.frag.
const float perceptualExponent = 0.454545438;

float weightAt(uint tap) {
    return plan.weights[tap / 4u][tap % 4u];
}

float lumaOf(vec3 colour) {
    return plan.lumaRow.x * colour.r + plan.lumaRow.y * colour.g + plan.lumaRow.z * colour.b;
}

float perceptualLuma(float luminance) {
    return luminance > 0.0 ? pow(luminance, perceptualExponent) : 0.0;
}

// The unit-luma ratio of decompose().
vec3 ratioOf(vec3 colour) {
    const float luminance = lumaOf(colour);
    const float scale = max(luminance, 0.0) + ratioFloor;
    const float offset = scale - luminance;
    return vec3((colour.r + offset * plan.neutral.x) / scale,
                (colour.g + offset * plan.neutral.y) / scale,
                (colour.b + offset * plan.neutral.z) / scale);
}

// texelFetch with the coordinate clamped to the image, as clampedAt().
vec4 clampedAt(ivec2 at) {
    const ivec2 last = textureSize(source, 0) - ivec2(1);
    return texelFetch(source, clamp(at, ivec2(0), last), 0);
}

// One grid cell: the plain mean of the block it covers, as a ratio.
vec3 reduce(ivec2 cell) {
    const int reduction = int(plan.gridReduction);
    const ivec2 size = ivec2(plan.sourceSize);
    vec3 sum = vec3(0.0);
    float count = 0.0;
    for (int dy = 0; dy < reduction; ++dy) {
        const int y = cell.y * reduction + dy;
        if (y >= size.y) {
            break;
        }
        for (int dx = 0; dx < reduction; ++dx) {
            const int x = cell.x * reduction + dx;
            if (x >= size.x) {
                break;
            }
            const vec3 pixel = texelFetch(source, ivec2(x, y), 0).rgb;
            sum.r += pixel.r;
            sum.g += pixel.g;
            sum.b += pixel.b;
            count += 1.0;
        }
    }
    return ratioOf(vec3(sum.r / count, sum.g / count, sum.b / count));
}

// blurPass(): a normalised Gaussian along one axis, edges clamped.
vec3 blurRatio(ivec2 at, ivec2 axis) {
    vec3 sum = clampedAt(at).rgb;
    float total = 1.0;
    for (uint tap = 1u; tap <= plan.radius; ++tap) {
        const float weight = weightAt(tap);
        const ivec2 offset = axis * int(tap);
        const vec3 after = clampedAt(at + offset).rgb;
        const vec3 before = clampedAt(at - offset).rgb;
        sum += weight * after + weight * before;
        total += 2.0 * weight;
    }
    return sum / total;
}

// The luminance a bilateral step reads at a pixel, and its perceptual value:
// worked out from the colour on the way across, and read from what across
// wrote on the way down, as the CPU precomputes its perceptual planes.
vec2 lumaAt(ivec2 at) {
    const vec4 texel = clampedAt(at);
    if (plan.step == stepBilateralAcross) {
        const float luminance = lumaOf(texel.rgb);
        return vec2(luminance, perceptualLuma(luminance));
    }
    return texel.rg;
}

// bilateralPass(): the centre weighs one, each tap its spatial weight times
// the edge-stop on its perceptual difference from the centre.
float bilateral(ivec2 at, ivec2 axis) {
    const vec2 centreTexel = lumaAt(at);
    const float centre = centreTexel.y;
    float sum = centreTexel.x;
    float total = 1.0;
    for (uint tap = 1u; tap <= plan.radius; ++tap) {
        const ivec2 offset = axis * int(tap);
        const vec2 after = lumaAt(at + offset);
        const vec2 before = lumaAt(at - offset);
        const float afterDifference = after.y - centre;
        const float beforeDifference = before.y - centre;
        const float afterWeight =
            weightAt(tap) * exp(-afterDifference * afterDifference * plan.rangeFactor);
        const float beforeWeight =
            weightAt(tap) * exp(-beforeDifference * beforeDifference * plan.rangeFactor);
        sum += afterWeight * after.x + beforeWeight * before.x;
        total += afterWeight + beforeWeight;
    }
    return sum / total;
}

void main() {
    // gl_FragCoord is the pixel's centre; its floor is the column and row.
    const ivec2 at = ivec2(floor(gl_FragCoord.xy));
    if (plan.step == stepReduce) {
        fragColor = vec4(reduce(at), 1.0);
    } else if (plan.step == stepBlurAcross) {
        fragColor = vec4(blurRatio(at, ivec2(1, 0)), 1.0);
    } else if (plan.step == stepBlurDown) {
        fragColor = vec4(blurRatio(at, ivec2(0, 1)), 1.0);
    } else if (plan.step == stepBilateralAcross) {
        // Luminance and its perceptual value, for down to read.
        const float luminance = bilateral(at, ivec2(1, 0));
        fragColor = vec4(luminance, perceptualLuma(luminance), 0.0, 1.0);
    } else {
        // Into a one-channel target where the device has one: only r is kept.
        fragColor = vec4(bilateral(at, ivec2(0, 1)), 0.0, 0.0, 1.0);
    }
}
