#version 440
// Focus Peaking edge-detection pass (docs/adr/0058): 3x3 Sobel gradient
// magnitude over the sRGB-encoded, post-adjustment luma produced by the
// preceding source pass (RendererCore::ensureFocusPeakingMask renders
// image.frag, overlays forced off, into an offscreen full-res target first).
// Shares nr.vert's passthrough vertex stage and NrUbuf layout (docs/adr/0034,
// 0046) rather than declaring a new uniform block: `strength` carries this
// pass's sensitivity threshold (docs/adr/0058), `invChroma` the source
// texture's texel size; sigma/radius/rangeSigma/amount are unused here but
// must stay declared — the block must match nr.vert exactly (std140), or the
// program fails to link with a duplicate-block-type error.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;

layout(std140, binding = 0) uniform nrbuf {
    mat4  clipCorr;
    vec2  invChroma;  // 1 / srcTex size, for the 3x3 tap offsets
    float sigma;      // unused here; the block must match nr.vert exactly
    int   radius;     // unused here; same reason
    int   flipV;      // unused here; same reason (nr.vert itself uses it)
    float strength;   // this pass's sensitivity threshold (docs/adr/0058)
    float rangeSigma; // unused here; same reason
    float amount;     // unused here; same reason
} u;

// Matches image.frag's kLuma (Rec.2020 weights) — srcTex already carries the
// sRGB-encoded (gamma-space) colour the source pass rendered, so this is a
// display-relative luma read, not a working-space one.
const vec3 kLuma = vec3(0.2627, 0.6780, 0.0593);

float luma(vec2 uv) {
    return dot(texture(srcTex, uv).rgb, kLuma);
}

void main() {
    vec2 t = u.invChroma;
    float tl = luma(vUV + vec2(-t.x, -t.y));
    float tc = luma(vUV + vec2( 0.0, -t.y));
    float tr = luma(vUV + vec2( t.x, -t.y));
    float ml = luma(vUV + vec2(-t.x,  0.0));
    float mr = luma(vUV + vec2( t.x,  0.0));
    float bl = luma(vUV + vec2(-t.x,  t.y));
    float bc = luma(vUV + vec2( 0.0,  t.y));
    float br = luma(vUV + vec2( t.x,  t.y));

    float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
    float gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);
    float mag = length(vec2(gx, gy)) * 0.25; // Sobel's own /4 normalisation

    fragColor = vec4(mag >= u.strength ? 1.0 : 0.0, 0.0, 0.0, 1.0);
}
