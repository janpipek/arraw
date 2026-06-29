#version 440
// Unified Noise Reduction recombine, full resolution (docs/adr/0034, 0046). Builds
// the denoised pixel from up to three inputs: the original colour, the blurred
// quarter-res chroma ratio, and the bilateral-smoothed full-res luma. Reduces
// exactly to the old chroma-only behaviour when Amount is 0, which guards the
// colour-NR regression.
//
//   Yf = mix(Y,        Y',  amount)     // luma half: edge-aware-smoothed luma
//   rf = mix(c/Y,      r',  strength)   // chroma half: smoothed chroma ratio
//   out = Yf * rf
//
// Each half is sampled only when its weight is positive, so an inactive leg's
// texture (which may be uninitialised) never poisons the result with NaNs.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;    // full-res raw slot
layout(binding = 2) uniform sampler2D chromaTex; // quarter-res blurred chroma ratio
layout(binding = 3) uniform sampler2D lumaTex;   // full-res bilateral-smoothed luma (.r)

layout(std140, binding = 0) uniform nrbuf {
    mat4  clipCorr;
    vec2  invChroma;
    float sigma;
    int   radius;
    int   flipV;
    float strength;
    float rangeSigma; // unused here; block must match nr.vert (std140)
    float amount;
} u;

const vec3 kLuma = vec3(0.2627, 0.6780, 0.0593);

void main() {
    vec3 c0 = texture(srcTex, vUV).rgb;
    float y = dot(c0, kLuma);
    vec3 ratio = y > 1e-6 ? c0 / y : vec3(1.0); // unit-luma chroma ratio

    float yf = y;
    if (u.amount > 0.0) {
        float yDenoised = texture(lumaTex, vUV).r; // bilateral output
        yf = mix(y, yDenoised, u.amount);
    }
    vec3 rf = ratio;
    if (u.strength > 0.0) {
        vec3 rb = texture(chromaTex, vUV).rgb; // bilinear upsample of blurred ratio
        rf = mix(ratio, rb, u.strength);
    }
    fragColor = vec4(yf * rf, 1.0);
}
