#version 440

// The vertical half of the resize of an opaque image: Resample.cpp's
// verticalPass<true>.
//
// Mirrors it, and must change with it; see resize_across_opaque.frag. The one
// plane the horizontal pass wrote is read, and the result is the filtered colour
// with alpha exactly one: no window is translucent, so there is no quotient by
// alpha and no clamp to a range of visible colours, and the float weight sums
// that could fall an ulp short of one are never looked at.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D sums;

// The contract with GpuResizeBlock: keep the members in step with it. The
// plane is not read here.
layout(std140, binding = 1) uniform Resize {
    uint plane;
    uint inputLength;
    uvec2 padding;
} plan;

// The weights of the vertical axis, as packResizeWeights lays them out.
layout(binding = 2) uniform sampler2D weights;

float tapWeight(int row, int k) {
    return texelFetch(weights, ivec2(1 + k / 4, row), 0)[k % 4];
}

float store(float value, float sawNegative) {
    return (value < 0.0 && sawNegative == 0.0) ? 0.0 : value;
}

void main() {
    // gl_FragCoord is (x + 0.5, y + 0.5): output column x of row y.
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const vec4 header = texelFetch(weights, ivec2(0, pixel.y), 0);
    const int first = int(header.x);
    const int count = int(header.y);
    const int last = int(plan.inputLength) - 1;

    vec3 sum = vec3(0.0);
    vec3 negative = vec3(0.0);
    for (int k = 0; k < count; ++k) {
        const float weight = tapWeight(pixel.y, k);
        const int row = clamp(first + k, 0, last);
        const vec3 across = texelFetch(sums, ivec2(pixel.x, row), 0).rgb;
        sum += weight * across;
        negative = max(negative, vec3(lessThan(across, vec3(0.0))));
    }
    fragColor = vec4(store(sum.r, negative.r), store(sum.g, negative.g),
                     store(sum.b, negative.b), 1.0);
}
