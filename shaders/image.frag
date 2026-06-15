#version 440
// Vulkan-dialect GLSL, compiled by qsb for every RHI backend (docs/adr/0006).
// The uniform block must match Ubuf in src/RendererCore.h and the copy in
// image.vert exactly (std140).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

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
} u;

layout(binding = 1) uniform sampler2D uTexture;
layout(binding = 2) uniform sampler2D uCurveLUT;   // 256×1 RGBA32F: R=luma, G=red, B=green, A=blue
layout(binding = 3) uniform sampler3D uLut3D;      // display LUT (soft-proof / monitor profile):
                                                   // indexed by sRGB-encoded working RGB,
                                                   // RGB=display output, A=in-gamut flag

// Rec.2020 luma — the whole pipeline works in linear Rec.2020 (docs/adr/0001).
// Must match kLumaR/G/B in src/ImagePipeline.h.
const vec3 kLuma = vec3(0.2627, 0.6780, 0.0593);

// Hue centres (0..1) for: Red, Orange, Yellow, Green, Aqua, Blue, Purple, Magenta
const float kHslCenters[8] = float[8](
    0.0,    // Red    (  0°)
    0.083,  // Orange ( 30°)
    0.167,  // Yellow ( 60°)
    0.333,  // Green  (120°)
    0.500,  // Aqua   (180°)
    0.611,  // Blue   (220°)
    0.778,  // Purple (280°)
    0.889   // Magenta(320°)
);

vec3 applyExposure(vec3 c) {
    return c * pow(2.0, u.exposure);
}

vec3 applyBaseLook(vec3 c) {
    float y = dot(c, kLuma);
    float y2 = mix(y, smoothstep(0.0, 1.0, y), 0.35);
    c *= y2 / max(y, 1e-5);

    float luma = dot(c, kLuma);
    float satBoost = 1.05 - 0.03 * smoothstep(0.45, 1.0, luma);
    return mix(vec3(luma), c, satBoost);
}

vec3 applyContrast(vec3 c) {
    if (abs(u.contrast) < 0.001) return c;
    return (c - 0.5) * (1.0 + u.contrast * 0.8) + 0.5;
}

vec3 applyToneRegions(vec3 c) {
    if (abs(u.highlights) < 0.001 && abs(u.shadows) < 0.001 &&
        abs(u.whites) < 0.001 && abs(u.blacks) < 0.001)
        return c;

    float y = dot(c, kLuma);

    // Luma masks for each tone region (hand-tuned ramp bounds).
    float hl = smoothstep(0.28, 0.88, y);
    float sh = 1.0 - smoothstep(0.12, 0.78, y);
    float w  = smoothstep(0.52, 0.97, y);
    float b  = 1.0 - smoothstep(0.03, 0.48, y);

    float delta = u.highlights * 0.5 * hl
                + u.shadows   * 0.5 * sh
                + u.whites    * 0.25 * w
                + u.blacks    * 0.25 * b;

    float y2 = max(y + delta, 0.0);
    return c * (y2 / max(y, 1e-5));
}

vec3 linearToSRGB(vec3 c) {
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

vec3 srgbToLinear(vec3 c) {
    return pow(clamp(c, 0.0, 1.0), vec3(2.2));
}

// Tone curve: sample RGBA LUT — R=luma, G=red, B=green, A=blue.
// Luma curve scales RGB proportionally (preserves hue); per-channel curves follow.
// Operates on gamma-encoded values (docs/adr/0003): the stored crs: points are
// display-space, matching Lightroom's interpretation of ToneCurvePV2012.
vec3 applyCurve(vec3 c) {
    c = linearToSRGB(c);
    float y  = dot(c, kLuma);
    float y2 = texture(uCurveLUT, vec2(y, 0.5)).r;
    if (y > 1e-5) c *= y2 / y;
    c.r = texture(uCurveLUT, vec2(clamp(c.r, 0.0, 1.0), 0.5)).g;
    c.g = texture(uCurveLUT, vec2(clamp(c.g, 0.0, 1.0), 0.5)).b;
    c.b = texture(uCurveLUT, vec2(clamp(c.b, 0.0, 1.0), 0.5)).a;
    return srgbToLinear(c);
}

vec3 applyTemperature(vec3 c) {
    float t = (u.temperature - 5500.0) / 5500.0;
    if (abs(t) < 0.0001) return c;
    c.r += t * 0.15;
    c.b -= t * 0.15;
    return c;
}

vec3 applyTint(vec3 c) {
    if (abs(u.tint) < 0.001) return c;
    c.g += u.tint * 0.05;
    return c;
}

// ── HSL ───────────────────────────────────────────────────────────────────────

vec3 rgb2hsv(vec3 c) {
    float maxC  = max(c.r, max(c.g, c.b));
    float minC  = min(c.r, min(c.g, c.b));
    float delta = maxC - minC;
    float h = 0.0;
    if (delta > 1e-5) {
        if (maxC == c.r)      h = (c.g - c.b) / delta + (c.g < c.b ? 6.0 : 0.0);
        else if (maxC == c.g) h = (c.b - c.r) / delta + 2.0;
        else                  h = (c.r - c.g) / delta + 4.0;
        h /= 6.0;
    }
    float s = (maxC > 1e-5) ? delta / maxC : 0.0;
    return vec3(h, s, maxC);
}

vec3 hsv2rgb(vec3 hsv) {
    float h = hsv.x * 6.0;
    float s = hsv.y;
    float v = hsv.z;
    int   i = int(floor(h));
    float f = h - float(i);
    float p = v * (1.0 - s);
    float q = v * (1.0 - s * f);
    float t = v * (1.0 - s * (1.0 - f));
    if (i == 0) return vec3(v, t, p);
    if (i == 1) return vec3(q, v, p);
    if (i == 2) return vec3(p, v, t);
    if (i == 3) return vec3(p, q, v);
    if (i == 4) return vec3(t, p, v);
    return vec3(v, p, q);
}

vec3 applyHsl(vec3 c) {
    vec3  hsv = rgb2hsv(c);
    float h   = hsv.x;

    float totalHue = 0.0;
    float totalSat = 0.0;
    float totalLum = 0.0;
    float totalW   = 0.0;

    for (int i = 0; i < 8; ++i) {
        float d = abs(h - kHslCenters[i]);
        if (d > 0.5) d = 1.0 - d;     // circular wrap
        float w = max(0.0, 1.0 - d * 6.0);  // cut off at ±60° (was 40°)
        w = w * w * (3.0 - 2.0 * w);  // smoothstep
        if (w > 0.001) {
            totalHue += u.hslHue[i >> 2][i & 3] * w;
            totalSat += u.hslSat[i >> 2][i & 3] * w;
            totalLum += u.hslLum[i >> 2][i & 3] * w;
            totalW   += w;
        }
    }

    if (totalW < 0.001) return c;

    float wInv = 1.0 / totalW;
    hsv.x = fract(hsv.x + totalHue * wInv / 12.0);                    // ±100 → max ±30°
    hsv.y = clamp(hsv.y * (1.0 + totalSat * wInv * 0.5), 0.0, 1.0);  // ±100 → ±50% sat
    hsv.z = clamp(hsv.z + totalLum * wInv * 0.5, 0.0, 1.0);          // ±100 → ±0.5 V

    return hsv2rgb(hsv);
}

vec3 applySaturation(vec3 c) {
    if (abs(u.saturation) < 0.001) return c;
    float luma = dot(c, kLuma);
    return mix(vec3(luma), c, 1.0 + u.saturation);
}

vec3 applyVibrance(vec3 c) {
    if (abs(u.vibrance) < 0.001) return c;
    float luma = dot(c, kLuma);
    float sat  = length(c - vec3(luma));
    return mix(vec3(luma), c, 1.0 + u.vibrance * (1.0 - sat));
}

// Display transform: linear Rec.2020 → sRGB display (docs/adr/0002).
// Columns of the Rec.2020 → Rec.709/sRGB primaries matrix (GLSL is column-major).
const mat3 kRec2020ToSRGB = mat3(
     1.6605, -0.1246, -0.0182,
    -0.5876,  1.1329, -0.1006,
    -0.0728, -0.0083,  1.1187
);

// True piecewise sRGB encode (not the 1/2.2 approximation).
vec3 srgbCurve(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

vec3 displayTransform(vec3 c) {
    return srgbCurve(clamp(kRec2020ToSRGB * c, 0.0, 1.0));
}

// Soft-proof / monitor-profile path: sample the lcms2-baked 3D LUT. The index
// is sRGB-encoded (same shaper the LUT was built with) so the grid resolution
// concentrates where the eye sees banding — the shadows.
vec3 displayLut(vec3 c) {
    vec3 idx = srgbCurve(clamp(c, 0.0, 1.0));
    float n = float(textureSize(uLut3D, 0).x);
    vec4 v = texture(uLut3D, idx * ((n - 1.0) / n) + 0.5 / n);
    if (u.gamutWarn != 0 && v.a < 0.5)
        return vec3(1.0, 0.1, 0.1);   // out-of-gamut warning overlay
    return v.rgb;
}

// Processing order (the authoritative definition — DESIGN.md mirrors it).
// Everything up to the final encode operates in linear Rec.2020:
//   base look → exposure → contrast → tone regions → tone curves
//   → temperature → tint → HSL → saturation → vibrance → display transform
// Crop/rotation happen earlier in image.vert. For export, displayEncode is
// 0: the offscreen readback stays in linear working space and the output
// transform runs on the CPU (lcms2, MainWindow::exportFile).
// With curveInput set, the pipeline stops after tone regions and
// gamma-encodes — the curve input histogram readback (docs/adr/0004).
void main() {
    if (any(lessThan(vUV, vec2(0.0))) || any(greaterThan(vUV, vec2(1.0)))) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 c = texture(uTexture, vUV).rgb;
    if (u.baseLook != 0)
        c = applyBaseLook(c);
    c = applyExposure(c);
    c = applyContrast(c);
    c = applyToneRegions(c);
    if (u.curveInput != 0) {
        fragColor = vec4(linearToSRGB(c), 1.0);
        return;
    }
    c = applyCurve(c);
    if (u.wbInput != 0) {
        fragColor = vec4(c, 1.0);   // linear value the WB picker inverts (pre-temp/tint)
        return;
    }
    c = applyTemperature(c);
    c = applyTint(c);
    if (u.hslActive != 0)
        c = applyHsl(c);
    c = applySaturation(c);
    c = applyVibrance(c);
    if (u.displayEncode == 0) {
        fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);          // export readback
        return;
    }

    vec3 outc = (u.useLut != 0) ? displayLut(c)             // proof / monitor ICC
                                : displayTransform(c);      // assume-sRGB display

    // Clipping overlay (docs/adr/0009): sRGB-relative, judged once here so it
    // works in both encode paths and the clip colour wins over the gamut-warning
    // red. Highlight red wins over shadow blue. clipWarn is forced 0 for the
    // export and histogram readbacks, so this only paints the on-screen frame.
    if (u.clipWarn != 0) {
        vec3 s = kRec2020ToSRGB * c;
        if ((u.clipWarn & 1) != 0 && any(greaterThanEqual(s, vec3(1.0))))
            outc = vec3(1.0, 0.0, 0.0);                     // highlight clip
        else if ((u.clipWarn & 2) != 0 && any(lessThanEqual(s, vec3(0.0))))
            outc = vec3(0.0, 0.0, 1.0);                     // shadow clip
    }
    fragColor = vec4(outc, 1.0);
}
