// What the four resize passes share: their uniform block, the weights they
// read, and Resample.cpp's tap weight and store. Included, never compiled on
// its own.

// The contract with GpuResizeBlock: keep the members in step with it
// (tests/gpu/test_GpuShaderLayout.cpp holds the two to it). inputLength is the
// length of the cut region, and offset where it starts in the source (ADR 025);
// tap indices are clamped to the region before the offset is added. The plane
// is read by the horizontal pass that keeps three (resize_across.frag) only.
layout(std140, binding = 1) uniform Resize {
    uint plane;
    uint inputLength;
    uvec2 offset;
} plan;

// The weights of the resized axis, as packResizeWeights lays them out: row i is
// output coordinate i, texel 0 is (first, count), the weights follow four to a
// texel.
layout(binding = 2) uniform sampler2D weights;

// Alpha below which a resampled pixel counts as fully transparent: the CPU's
// transparentBelow (2^-16). Read by the passes that keep alpha.
const float transparentBelow = 1.0 / 65536.0;

// Stands for the CPU's infinity, which some drivers flush or compare oddly. A
// window with no visible pixel keeps low above high, which is all the CPU's
// infinities mean to the vertical pass. Read by the passes that keep alpha.
const float floatMax = 3.4028235e38;

// The weight of tap k of an output coordinate: axis.weights[taps.offset + k].
float tapWeight(int row, int k) {
    return texelFetch(weights, ivec2(1 + k / 4, row), 0)[k % 4];
}

// The CPU's store(): an accumulated sample, with the rule that ringing never
// goes below black. sawNegative is 1 if any input of the sum was negative.
float store(float value, float sawNegative) {
    return (value < 0.0 && sawNegative == 0.0) ? 0.0 : value;
}
