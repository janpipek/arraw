#version 440
// Luminance NR bilateral, horizontal pass (docs/adr/0046). The dual of the chroma
// blur: an edge-aware separable Gaussian over luma Y. Reads the full-res raw colour
// and reduces it to Y = dot(c, kLuma) inline, so no separate luma-extract pass is
// needed. Taps are weighted by a spatial Gaussian (sigma) times a range Gaussian on
// the *perceptual* luma difference (tone::kGamma encoding), so a tap across a strong
// luma edge is down-weighted — flat noise smooths while edges survive. Output is the
// scalar blurred Y (in every channel); the vertical pass refines it.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex; // full-res raw colour

layout(std140, binding = 0) uniform nrbuf {
    mat4  clipCorr;
    vec2  invChroma;  // 1 / full-res size (this buffer carries the luma context)
    float sigma;      // spatial sigma in full-res pixels
    int   radius;     // spatial tap radius (capped)
    int   flipV;
    float strength;   // unused here; block must match nr.vert (std140)
    float rangeSigma; // perceptual range sigma (edge-stop)
    float amount;     // unused here
} u;

const vec3 kLuma = vec3(0.2627, 0.6780, 0.0593);
const float kGamma = 2.2; // tone::kGamma (develop/BasicTone.h, docs/adr/0046)
const int kMaxRadius = 64;

float enc(float y) { return pow(max(y, 0.0), 1.0 / kGamma); } // perceptual encode

void main() {
    float yc = dot(texture(srcTex, vUV).rgb, kLuma);
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
        vec2 off = vec2(float(i) * u.invChroma.x, 0.0);
        float yp = dot(texture(srcTex, vUV + off).rgb, kLuma);
        float yn = dot(texture(srcTex, vUV - off).rgb, kLuma);
        float dp = enc(yp) - ec;
        float dn = enc(yn) - ec;
        float wp = ws * exp(-dp * dp * invR);
        float wn = ws * exp(-dn * dn * invR);
        acc += wp * yp + wn * yn;
        wsum += wp + wn;
    }
    fragColor = vec4(vec3(acc / wsum), 1.0);
}
