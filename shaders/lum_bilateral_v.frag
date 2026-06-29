#version 440
// Luminance NR bilateral, vertical pass (docs/adr/0046). Refines the horizontal
// pass's scalar luma (in .r) along the other axis with the same edge-aware weights —
// a separable approximation of a 2D bilateral. Range weights are evaluated on the
// already-H-blurred luma, the standard separable-bilateral trade. Output is the
// denoised luma Y' the recombine pass blends in by Amount.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex; // full-res H-blurred luma (.r)

layout(std140, binding = 0) uniform nrbuf {
    mat4  clipCorr;
    vec2  invChroma;  // 1 / full-res size
    float sigma;      // spatial sigma in full-res pixels
    int   radius;
    int   flipV;
    float strength;   // unused here; block must match nr.vert (std140)
    float rangeSigma; // perceptual range sigma (edge-stop)
    float amount;     // unused here
} u;

const float kGamma = 2.2; // tone::kGamma (develop/BasicTone.h, docs/adr/0046)
const int kMaxRadius = 64;

float enc(float y) { return pow(max(y, 0.0), 1.0 / kGamma); } // perceptual encode

void main() {
    float yc = texture(srcTex, vUV).r;
    float ec = enc(yc);
    float s = max(u.sigma, 1e-4);
    float invS = 1.0 / (2.0 * s * s);
    float r = max(u.rangeSigma, 1e-4);
    float invR = 1.0 / (2.0 * r * r);

    float acc = yc;
    float wsum = 1.0;
    for (int i = 1; i <= kMaxRadius; ++i) {
        if (i > u.radius)
            break;
        float ws = exp(-float(i * i) * invS);
        vec2 off = vec2(0.0, float(i) * u.invChroma.y);
        float yp = texture(srcTex, vUV + off).r;
        float yn = texture(srcTex, vUV - off).r;
        float dp = enc(yp) - ec;
        float dn = enc(yn) - ec;
        float wp = ws * exp(-dp * dp * invR);
        float wn = ws * exp(-dn * dn * invR);
        acc += wp * yp + wn * yn;
        wsum += wp + wn;
    }
    fragColor = vec4(vec3(acc / wsum), 1.0);
}
