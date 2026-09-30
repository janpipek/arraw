#version 440

// The geometry resample: GeometryPlan.cpp's applyGeometry and sample.
//
// Placeholder until step 4 of docs/ideas/gpu-develop-plan.md: the block is
// the contract (GpuGeometryBlock in GpuPlan.h), the body only nearest
// neighbour.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D source;

layout(std140, binding = 1) uniform Geometry {
    vec2 origin;
    vec2 columnStep;
    vec2 rowStep;
    uvec2 sourceSize;
    uvec2 outputSize;
} plan;

void main() {
    const vec2 position = plan.origin + gl_FragCoord.x * plan.columnStep +
                          gl_FragCoord.y * plan.rowStep;
    const ivec2 texel = clamp(ivec2(floor(position)), ivec2(0), ivec2(plan.sourceSize) - 1);
    fragColor = texelFetch(source, texel, 0);
}
