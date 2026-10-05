#version 440

// The recombination that ends the Denoise pass: Denoise.cpp's applyDenoise
// after its filters. Each pixel is split into luminance and unit-luma ratio,
// the luminance moved toward the filtered one, the ratio toward the blurred
// grid read bilinearly, and the two put back together.
//
// Every function below mirrors the C++ of the same name in src/core/Denoise.cpp
// and must change with it; the block is GpuDenoiseBlock's (GpuPlan.h), as in
// denoise_filter.frag. Alpha goes through untouched.
//
// Parity with the CPU, and the tolerance the tests hold, are measured on
// Vulkan (lavapipe) only.

layout(location = 0) out vec4 fragColor;

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
// The blurred ratios on the grid; the source when colour is not smoothed.
layout(binding = 2) uniform sampler2D grid;
// The filtered luminance in r; the source when luminance is not smoothed.
layout(binding = 3) uniform sampler2D filtered;

// denoiseRatioFloor in Denoise.h: 2^-14.
const float ratioFloor = 6.103515625e-05;

float lumaOf(vec3 colour) {
    return plan.lumaRow.x * colour.r + plan.lumaRow.y * colour.g + plan.lumaRow.z * colour.b;
}

vec3 decomposeRatio(vec3 colour, float luminance) {
    const float scale = max(luminance, 0.0) + ratioFloor;
    const float offset = scale - luminance;
    return vec3((colour.r + offset * plan.neutral.x) / scale,
                (colour.g + offset * plan.neutral.y) / scale,
                (colour.b + offset * plan.neutral.z) / scale);
}

vec3 recompose(float luminance, vec3 ratio) {
    const float scale = max(luminance, 0.0) + ratioFloor;
    const float offset = scale - luminance;
    return vec3(scale * ratio.r - offset * plan.neutral.x, scale * ratio.g - offset * plan.neutral.y,
                scale * ratio.b - offset * plan.neutral.z);
}

// gridTap(): u = (x + 0.5) / reduction - 0.5, clamped to the grid; exact in
// float, the reduction being a power of two.
void gridTap(int coordinate, uint cells, out int first, out int second, out float fraction) {
    const float position = (float(coordinate) + 0.5) / float(plan.gridReduction) - 0.5;
    const float clamped = clamp(position, 0.0, float(cells - 1u));
    const float below = floor(clamped);
    first = int(below);
    second = min(first + 1, int(cells) - 1);
    fraction = clamped - below;
}

// upsampled(): the grid read bilinearly, rows first.
vec3 upsampled(ivec2 at) {
    int x0;
    int x1;
    float tx;
    int y0;
    int y1;
    float ty;
    gridTap(at.x, plan.gridSize.x, x0, x1, tx);
    gridTap(at.y, plan.gridSize.y, y0, y1, ty);
    const vec3 topLeft = texelFetch(grid, ivec2(x0, y0), 0).rgb;
    const vec3 topRight = texelFetch(grid, ivec2(x1, y0), 0).rgb;
    const vec3 bottomLeft = texelFetch(grid, ivec2(x0, y1), 0).rgb;
    const vec3 bottomRight = texelFetch(grid, ivec2(x1, y1), 0).rgb;
    const vec3 top = topLeft + tx * (topRight - topLeft);
    const vec3 bottom = bottomLeft + tx * (bottomRight - bottomLeft);
    return top + ty * (bottom - top);
}

void main() {
    const ivec2 at = ivec2(floor(gl_FragCoord.xy));
    const vec4 texel = texelFetch(source, at, 0);
    float luminance = lumaOf(texel.rgb);
    vec3 ratio = decomposeRatio(texel.rgb, luminance);
    if (plan.luminance != 0u) {
        luminance += plan.luminanceMix * (texelFetch(filtered, at, 0).r - luminance);
    }
    if (plan.color != 0u) {
        ratio += plan.colorMix * (upsampled(at) - ratio);
    }
    fragColor = vec4(recompose(luminance, ratio), texel.a);
}
