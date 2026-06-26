#version 440
// Colour NR pre-pass 3: recombine at full resolution (docs/adr/0032, issue #59).
// Takes the original full-res luma and the blurred quarter-res chroma (bilinearly
// upsampled by the sampler), producing the denoised image the main pipeline samples
// in place of the raw slot. Strength blends the denoised chroma back over the raw
// colour. Luma is preserved exactly at any Strength (both the raw and blurred
// ratios carry luma 1, so their mix does too); only colour is smoothed.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;    // full-res raw slot
layout(binding = 2) uniform sampler2D chromaTex; // quarter-res blurred chroma ratio

// Shares the `nrbuf` prefix with nr.vert (std140); this stage additionally reads
// the trailing `strength`, which the other passes don't declare. Std140 keeps the
// shared members at the same offsets, so the buffer stays compatible.
layout(std140, binding = 0) uniform nrbuf {
    mat4  clipCorr;
    vec2  invChroma;
    float sigma;
    int   radius;
    int   flipV;
    float strength; // recombine blend factor 0..1 (Strength)
} u;

const vec3 kLuma = vec3(0.2627, 0.6780, 0.0593);

void main() {
    vec3 c0 = texture(srcTex, vUV).rgb;
    float y = dot(c0, kLuma);
    vec3 rb = texture(chromaTex, vUV).rgb; // bilinear upsample
    // mix(c0, y*rb, strength): Strength 1 = fully denoised, 0 = untouched.
    fragColor = vec4(mix(c0, y * rb, u.strength), 1.0);
}
