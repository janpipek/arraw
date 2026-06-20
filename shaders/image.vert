#version 440
// Vulkan-dialect GLSL, compiled by qsb for every RHI backend (docs/adr/0006).
// The uniform block must match Ubuf in src/RendererCore.h and the copy in
// image.frag exactly (std140).

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform buf {
    mat4  clipCorr;      // QRhi::clipSpaceCorrMatrix() — GL-style NDC → backend NDC
    vec4  transform;     // (scaleX, scaleY, panX, panY)
    vec4  cropRect;      // UV bounds: (left, top, right, bottom)
    vec4  hslHue[2];     // 8 floats, -1..+1 per range (std140: packed as vec4 pairs)
    vec4  hslSat[2];
    vec4  hslLum[2];
    float rotation;      // degrees
    float aspect;        // crop width / height in pixels (for isotropic rotation)
    float exposure;      // EV stops
    float contrast;      // -0.2..+0.2 (slider ±100 / kToneSliderToUniform)
    float highlights;    // -0.2..+0.2
    float shadows;       // -0.2..+0.2
    float whites;        // -0.2..+0.2
    float blacks;        // -0.2..+0.2
    float temperature;   // Kelvin, 2000..12000 (5500 = neutral)
    float tint;          // -1..+1
    float saturation;    // -1..+1
    float vibrance;      // -1..+1
    int   useLut;
    int   gamutWarn;
    int   baseLook;
    int   displayEncode; // 1: encode for the (assumed sRGB) display;
                         // 0: output clamped linear working space (export readback)
    int   curveInput;    // stop after tone regions + gamma-encode (histograms)
    int   hslActive;
    int   wbInput;       // stop before temperature/tint, output linear (WB picker)
    int   clipWarn;      // clipping overlay bits: 1 = highlights, 2 = shadows (docs/adr/0009)
    // Local adjustments (docs/adr/0010) — unused here, but the block must match
    // image.frag and Ubuf byte-for-byte (std140).
    vec4  laGeom[16];
    vec4  laTone[16];
    vec4  laTone2[16];
    vec4  laColor[16];
    vec4  laGeom2[16];
    int   numLocalAdj;
    int   histoRaw;      // unused here; present so the block matches image.frag and Ubuf
} u;

void main() {
    vec2 p = aPos * u.transform.xy + u.transform.zw;
    gl_Position = u.clipCorr * vec4(p, 0.0, 1.0);

    // Map quad UV to the crop region, rotate around image centre (fixed pivot)
    vec2 uv = mix(u.cropRect.xy, u.cropRect.zw, aUV);
    vec2 center = vec2(0.5, 0.5);
    vec2 d = uv - center;
    d.x *= u.aspect;
    float rad = u.rotation * 3.14159265 / 180.0;
    float c   = cos(rad);
    float s   = sin(rad);
    vec2 r = vec2(c * d.x - s * d.y, s * d.x + c * d.y);
    r.x /= u.aspect;
    vUV = r + center;
}
