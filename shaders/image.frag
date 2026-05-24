#version 330 core

in  vec2 vUV;
out vec4 fragColor;

uniform sampler2D uTexture;

uniform float uExposure;     // EV stops
uniform float uContrast;     // -1..+1
uniform float uHighlights;   // -1..+1
uniform float uShadows;      // -1..+1
uniform float uWhites;       // -1..+1
uniform float uBlacks;       // -1..+1
uniform float uTemperature;  // Kelvin, 2000..12000 (5500 = neutral)
uniform float uTint;         // -1..+1
uniform float uSaturation;   // -1..+1
uniform float uVibrance;     // -1..+1

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

vec3 applyExposure(vec3 c) {
    return c * pow(2.0, uExposure);
}

vec3 applyContrast(vec3 c) {
    if (abs(uContrast) < 0.001) return c;
    return (c - 0.5) * (1.0 + uContrast * 0.8) + 0.5;
}

// Luma-masked tone regions: wide overlapping ranges, single luminance remap
// (preserves hue, avoids per-channel mask seams and curve inversions).
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
                + uShadows  * 0.5 * sh
                + uWhites   * 0.25 * w
                + uBlacks   * 0.25 * b;

    float y2 = max(y + delta, 0.0);
    return c * (y2 / max(y, 1e-5));
}

vec3 applyTemperature(vec3 c) {
    // Normalise around 5500K: positive = warmer (more red, less blue)
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
    // Clamp UV — pixels outside [0,1] (due to rotation) show black
    if (any(lessThan(vUV, vec2(0.0))) || any(greaterThan(vUV, vec2(1.0)))) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 c = texture(uTexture, vUV).rgb;
    c = applyExposure(c);
    c = applyContrast(c);
    c = applyToneRegions(c);
    c = applyTemperature(c);
    c = applyTint(c);
    c = applySaturation(c);
    c = applyVibrance(c);
    fragColor = vec4(linearToSRGB(c), 1.0);
}
