#version 440

// The Effects pass: Effects.cpp's effectsPixel, effect for effect.
//
// Every function below mirrors the C++ of the same name in src/core/Effects.cpp
// (and, for the helpers, src/core/ProcessingPlan.h), in the same order and with
// the same arithmetic, and must change with it. The Effects block is the
// contract with GpuEffectsBlock in GpuPlan.h: same members, same order.
//
// Runs on the resized image, so each pixel's place in the crop frame is the
// block's origin plus its centre times the step (ADR 037). Alpha goes through
// untouched.
//
// Parity with the CPU, and the tolerance the tests hold, are measured on
// Vulkan (lavapipe) only.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D source;

// GrainLayer in GrainModels.h: one lattice placed on the output pixels.
struct GrainLayer {
    ivec2 cell;
    vec2 fraction;
    vec2 delta;
    float weight;
    uint seed;
};

layout(std140, binding = 1) uniform Effects {
    vec2 origin;
    vec2 step;
    uint vignettes;
    uint vignetteLightens;
    uint vignetteHardEdge;
    float vignetteStops;
    float vignetteInner;
    float vignetteOuter;
    uint grains;
    uint grainModel;
    GrainLayer grainLayers[4];
} plan;

// 1 / 2.2f and 2.2f as C++ rounds them to float; see develop.frag.
const float perceptualExponent = 0.454545438;
const float linearExponent = 2.20000005;

// std::pow for the arguments given it; see develop.frag.
float pow0(float x, float y) {
    if (x > 0.0) {
        return pow(x, y);
    }
    if (x == 0.0) {
        return 0.0;
    }
    return uintBitsToFloat(0x7fc00000u);
}

// std::clamp, leaving a NaN alone; see develop.frag.
float clampExact(float value, float low, float high) {
    return value < low ? low : (high < value ? high : value);
}

// smoothstep, ProcessingPlan.h; not the built-in.
float smoothStep(float first, float last, float value) {
    const float t = clampExact((value - first) / (last - first), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// toPerceptualSigned and fromPerceptualSigned, ProcessingPlan.h.
float toPerceptualSigned(float value) {
    return value < 0.0 ? -pow0(-value, perceptualExponent) : pow0(value, perceptualExponent);
}

float fromPerceptualSigned(float value) {
    return value < 0.0 ? -pow0(-value, linearExponent) : pow0(value, linearExponent);
}

float vignetteWeight(vec2 point) {
    const float across = 2.0 * point.x - 1.0;
    const float down = 2.0 * point.y - 1.0;
    const float radius = sqrt((across * across + down * down) * 0.5);
    if (plan.vignetteHardEdge != 0u) {
        return radius >= plan.vignetteInner ? 1.0 : 0.0;
    }
    return smoothStep(plan.vignetteInner, plan.vignetteOuter, radius);
}

float screen(float value, float keep) {
    return fromPerceptualSigned(1.0 - (1.0 - toPerceptualSigned(value)) * keep);
}

vec3 applyVignette(vec3 colour, float weight) {
    if (!(weight > 0.0)) {
        return colour;
    }
    if (plan.vignetteLightens == 0u) {
        const float gain = exp2(-plan.vignetteStops * weight);
        return vec3(colour.x * gain, colour.y * gain, colour.z * gain);
    }
    const float keep = exp2(-plan.vignetteStops * weight / 2.2);
    return vec3(screen(colour.x, keep), screen(colour.y, keep), screen(colour.z, keep));
}

// Grain models, GrainModels.cpp. Everything from the lattice position to the
// grain is exact float or integer arithmetic, one correctly rounded operation
// at a time (Vulkan rounds +, - and * correctly): `precise` forbids fusing them
// into fma, which the CPU does not do either, so both backends find the same
// cells and the same grain bit for bit. Only applyGrain's powers differ.

uint grainHash(uint x, uint y, uint seed) {
    uint h = (x * 0x8da6b343u) ^ (y * 0xd8163841u) ^ (seed * 0xcb1ab31fu);
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    return h ^ (h >> 16u);
}

float latticeValue(uint x, uint y, uint seed) {
    const int centred = int(grainHash(x, y, seed) >> 8u) - 0x800000;
    // 0x1p-24, a power of two: the product is exact.
    return (float(centred) + 0.5) * 5.9604644775390625e-8;
}

float layerValue(GrainLayer layer, vec2 pixel) {
    precise const vec2 p = layer.fraction + pixel * layer.delta;
    const vec2 f = floor(p);
    const uvec2 c = uvec2(layer.cell) + uvec2(ivec2(f));
    precise const vec2 t = p - f;
    precise const vec2 s = t * t * (3.0 - 2.0 * t);
    const float a = latticeValue(c.x, c.y, layer.seed);
    const float b = latticeValue(c.x + 1u, c.y, layer.seed);
    const float cc = latticeValue(c.x, c.y + 1u, layer.seed);
    const float d = latticeValue(c.x + 1u, c.y + 1u, layer.seed);
    precise const float top = a + (b - a) * s.x;
    precise const float bottom = cc + (d - cc) * s.x;
    precise const float value = top + (bottom - top) * s.y;
    return value;
}

float valueNoiseGrain(vec2 pixel) {
    precise float grain = 0.0;
    for (int index = 0; index < 4; ++index) {
        if (plan.grainLayers[index].weight != 0.0) {
            grain = grain + plan.grainLayers[index].weight * layerValue(plan.grainLayers[index], pixel);
        }
    }
    return grain;
}

// grainAt: the model the block names; the pass knows no model.
float grainAt(vec2 pixel) {
    switch (plan.grainModel) {
    case 0u: // GrainModel::ValueNoise
        return valueNoiseGrain(pixel);
    }
    return 0.0;
}

vec3 applyGrain(vec3 colour, float grain) {
    if (grain == 0.0) {
        return colour;
    }
    return vec3(fromPerceptualSigned(toPerceptualSigned(colour.x) + grain),
                fromPerceptualSigned(toPerceptualSigned(colour.y) + grain),
                fromPerceptualSigned(toPerceptualSigned(colour.z) + grain));
}

vec3 effectsPixel(vec2 pixel, vec3 colour) {
    if (plan.vignettes != 0u) {
        // The pixel's centre in the crop frame.
        const vec2 point = plan.origin + (pixel + 0.5) * plan.step;
        colour = applyVignette(colour, vignetteWeight(point));
    }
    if (plan.grains != 0u) {
        colour = applyGrain(colour, grainAt(pixel));
    }
    return colour;
}

void main() {
    // gl_FragCoord is the pixel's centre, (x + 0.5, y + 0.5); its floor is the
    // pixel's column and row, exactly.
    const vec2 pixel = floor(gl_FragCoord.xy);
    const vec4 texel = texelFetch(source, ivec2(pixel), 0);
    fragColor = vec4(effectsPixel(pixel, texel.rgb), texel.a);
}
