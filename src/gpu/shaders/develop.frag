#version 440

// The pointwise chain: ProcessingPlan.h's developPixel, stage for stage.
//
// Placeholder until step 3 of docs/ideas/gpu-develop-plan.md: the block is
// the contract (GpuPointwiseBlock in GpuPlan.h), the body only a copy.

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

void main() {
    const vec4 texel = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
    fragColor = vec4(texel.rgb * plan.exposureGain, texel.a);
}
