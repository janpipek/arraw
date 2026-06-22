#version 440
// Colour NR pre-pass 2b: vertical separable Gaussian over the quarter-res chroma
// ratio (docs/adr/0032). Mirror of nr_blur_h with a vertical tap offset.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;

layout(std140, binding = 0) uniform nrbuf {
    mat4  clipCorr;
    vec2  invChroma;
    float sigma;
    int   radius;
    int   flipV; // unused here; the block must match nr.vert exactly (std140)
} u;

const int kMaxRadius = 64;

void main() {
    float s = max(u.sigma, 1e-4);
    float inv2s2 = 1.0 / (2.0 * s * s);
    vec3 acc = texture(srcTex, vUV).rgb;
    float wsum = 1.0;
    for (int i = 1; i <= kMaxRadius; ++i) {
        if (i > u.radius)
            break;
        float w = exp(-float(i * i) * inv2s2);
        vec2 off = vec2(0.0, float(i) * u.invChroma.y);
        acc += w * texture(srcTex, vUV + off).rgb;
        acc += w * texture(srcTex, vUV - off).rgb;
        wsum += 2.0 * w;
    }
    fragColor = vec4(acc / wsum, 1.0);
}
