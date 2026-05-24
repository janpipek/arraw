#version 330 core

in  vec2 vUV;
out vec4 fragColor;

uniform sampler2D uTexture;
uniform sampler2D uCurveLUT;    // 256×1 RGBA32F: R=luma, G=red, B=green, A=blue

uniform float uExposure;        // EV stops
uniform float uContrast;        // -1..+1
uniform float uHighlights;      // -1..+1
uniform float uShadows;         // -1..+1
uniform float uWhites;          // -1..+1
uniform float uBlacks;          // -1..+1
uniform float uTemperature;     // Kelvin, 2000..12000 (5500 = neutral)
uniform float uTint;            // -1..+1
uniform float uSaturation;      // -1..+1
uniform float uVibrance;        // -1..+1
uniform bool  uBaseLook;
uniform bool  uHslActive;
uniform float uHslHue[8];       // -1..+1 per range
uniform float uHslSat[8];
uniform float uHslLum[8];

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

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
    return c * pow(2.0, uExposure);
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
    if (abs(uContrast) < 0.001) return c;
    return (c - 0.5) * (1.0 + uContrast * 0.8) + 0.5;
}

vec3 applyToneRegions(vec3 c) {
    if (abs(uHighlights) < 0.001 && abs(uShadows) < 0.001 &&
        abs(uWhites) < 0.001 && abs(uBlacks) < 0.001)
        return c;

    float y = dot(c, kLuma);

    float hl = smoothstep(0.28, 0.88, y);
    float sh = 1.0 - smoothstep(0.12, 0.78, y);
    float w  = smoothstep(0.52, 0.97, y);
    float b  = 1.0 - smoothstep(0.03, 0.48, y);

    float delta = uHighlights * 0.5 * hl
                + uShadows   * 0.5 * sh
                + uWhites    * 0.25 * w
                + uBlacks    * 0.25 * b;

    float y2 = max(y + delta, 0.0);
    return c * (y2 / max(y, 1e-5));
}

// Tone curve: sample RGBA LUT — R=luma, G=red, B=green, A=blue.
// Luma curve scales RGB proportionally (preserves hue); per-channel curves follow.
vec3 applyCurve(vec3 c) {
    float y  = dot(c, kLuma);
    float y2 = texture(uCurveLUT, vec2(y, 0.5)).r;
    if (y > 1e-5) c *= y2 / y;
    c.r = texture(uCurveLUT, vec2(clamp(c.r, 0.0, 1.0), 0.5)).g;
    c.g = texture(uCurveLUT, vec2(clamp(c.g, 0.0, 1.0), 0.5)).b;
    c.b = texture(uCurveLUT, vec2(clamp(c.b, 0.0, 1.0), 0.5)).a;
    return c;
}

vec3 applyTemperature(vec3 c) {
    float t = (uTemperature - 5500.0) / 5500.0;
    if (abs(t) < 0.0001) return c;
    c.r += t * 0.15;
    c.b -= t * 0.15;
    return c;
}

vec3 applyTint(vec3 c) {
    if (abs(uTint) < 0.001) return c;
    c.g += uTint * 0.05;
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
            totalHue += uHslHue[i] * w;
            totalSat += uHslSat[i] * w;
            totalLum += uHslLum[i] * w;
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
    if (abs(uSaturation) < 0.001) return c;
    float luma = dot(c, kLuma);
    return mix(vec3(luma), c, 1.0 + uSaturation);
}

vec3 applyVibrance(vec3 c) {
    if (abs(uVibrance) < 0.001) return c;
    float luma = dot(c, kLuma);
    float sat  = length(c - vec3(luma));
    return mix(vec3(luma), c, 1.0 + uVibrance * (1.0 - sat));
}

vec3 linearToSRGB(vec3 c) {
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

void main() {
    if (any(lessThan(vUV, vec2(0.0))) || any(greaterThan(vUV, vec2(1.0)))) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 c = texture(uTexture, vUV).rgb;
    if (uBaseLook)
        c = applyBaseLook(c);
    c = applyExposure(c);
    c = applyContrast(c);
    c = applyToneRegions(c);
    c = applyCurve(c);
    c = applyTemperature(c);
    c = applyTint(c);
    if (uHslActive)
        c = applyHsl(c);
    c = applySaturation(c);
    c = applyVibrance(c);
    fragColor = vec4(linearToSRGB(c), 1.0);
}
