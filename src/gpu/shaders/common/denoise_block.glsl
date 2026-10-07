// The Denoise passes' uniform block and the ratio floor, shared by
// denoise_filter.frag and denoise_combine.frag. Included, never compiled on
// its own.
//
// The Denoise block is the contract with GpuDenoiseBlock in GpuPlan.h: same
// members, same order (tests/gpu/test_GpuShaderLayout.cpp holds the two to it).
// One block for every step: each reads what it needs.

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

// denoiseRatioFloor in Denoise.h: 2^-14.
const float ratioFloor = 6.103515625e-05;
