#version 440
// Colour NR pre-pass 2a: horizontal separable Gaussian over the quarter-res
// chroma ratio (docs/adr/0032). Direction is hardcoded so the nrbuf uniform is
// constant across all NR passes in a frame. Weights from sigma; the loop is
// bounded by a compile-time cap and breaks at the real radius (GLSL ES 3.00).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;

layout(std140, binding = 0) uniform nrbuf {
    mat4  clipCorr;
    vec2  invChroma;
    float sigma;
    int   radius;
    int   flipV;     // unused here; the block must match nr.vert exactly (std140)
    float strength;  // unused here; same reason
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
        vec2 off = vec2(float(i) * u.invChroma.x, 0.0);
        acc += w * texture(srcTex, vUV + off).rgb;
        acc += w * texture(srcTex, vUV - off).rgb;
        wsum += 2.0 * w;
    }
    fragColor = vec4(acc / wsum, 1.0);
}
