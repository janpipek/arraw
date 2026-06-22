#version 440
// Colour NR pre-pass 3: recombine at full resolution (docs/adr/0032). Takes the
// original full-res luma and the blurred quarter-res chroma (bilinearly upsampled
// by the sampler), producing the denoised image the main pipeline samples in
// place of the raw slot. Luma is preserved exactly; only colour is smoothed.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;    // full-res raw slot
layout(binding = 2) uniform sampler2D chromaTex; // quarter-res blurred chroma ratio

const vec3 kLuma = vec3(0.2627, 0.6780, 0.0593);

void main() {
    vec3 c0 = texture(srcTex, vUV).rgb;
    float y = dot(c0, kLuma);
    vec3 rb = texture(chromaTex, vUV).rgb; // bilinear upsample
    fragColor = vec4(y * rb, 1.0);
}
