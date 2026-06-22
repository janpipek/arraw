#version 440
// Colour NR pre-pass 1: extract chroma at reduced resolution (docs/adr/0032).
// Rendered into a quarter-size target, so linear filtering of the full-res source
// already averages a 2×2 neighbourhood. Outputs the unit-luma chroma ratio c/Y;
// the subsequent blur smooths it and the recombine pass multiplies by full-res Y,
// so per-pixel luma is preserved and only colour is touched.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;

const vec3 kLuma = vec3(0.2627, 0.6780, 0.0593); // Rec.2020 (ImagePipeline.h kLuma*)

void main() {
    vec3 c = texture(srcTex, vUV).rgb;
    float y = dot(c, kLuma);
    fragColor = vec4(y > 1e-6 ? c / y : vec3(1.0), 1.0); // neutral ratio for near-black
}
