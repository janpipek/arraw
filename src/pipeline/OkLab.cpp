#include "pipeline/OkLab.h"

#include <algorithm>
#include <cmath>

namespace colour {
namespace {

// Rec.2020 luminance weights — must match kLuma in shaders/image.frag and
// kLumaR/G/B in ImagePipeline.h (docs/adr/0001).
constexpr float kLumaR = 0.2627f;
constexpr float kLumaG = 0.6780f;
constexpr float kLumaB = 0.0593f;

float luma(const Rgb& c) {
    return kLumaR * c[0] + kLumaG * c[1] + kLumaB * c[2];
}

// Oklab is defined from linear sRGB/Rec.709 (Ottosson 2020). arraw works in
// linear Rec.2020, so we convert Rec.2020 -> Rec.709 first. These two primaries
// matrices are mutual inverses (to ~6 decimals), which is what makes the Oklab
// round trip exact; the shader mirrors the same constants.
Rgb rec2020ToRec709(const Rgb& c) {
    return {
        1.660491f * c[0] - 0.587641f * c[1] - 0.072850f * c[2],
        -0.124550f * c[0] + 1.132900f * c[1] - 0.008349f * c[2],
        -0.018151f * c[0] - 0.100579f * c[1] + 1.118730f * c[2],
    };
}

Rgb rec709ToRec2020(const Rgb& c) {
    return {
        0.627404f * c[0] + 0.329283f * c[1] + 0.043313f * c[2],
        0.069097f * c[0] + 0.919541f * c[1] + 0.011362f * c[2],
        0.016391f * c[0] + 0.088013f * c[1] + 0.895595f * c[2],
    };
}

// Vibrance weight falls off with current chroma: muted colours weigh ~1, vivid
// ones tail toward 0 (but never reach it, so a positive Vibrance always nudges).
// kVibranceHalf is the Oklab chroma at which the weight is one half.
constexpr float kVibranceHalf = 0.2f;

// Path to White exponent: >1 so only deep highlights bleach toward white
// (docs/adr/0035).
constexpr float kPathToWhite = 1.5f;

} // namespace

Lab toOklab(const Rgb& rgb) {
    const Rgb c = rec2020ToRec709(rgb);
    const float l = 0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2];
    const float m = 0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2];
    const float s = 0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2];
    const float l_ = std::cbrt(l);
    const float m_ = std::cbrt(m);
    const float s_ = std::cbrt(s);
    return {
        0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
        1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
        0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
    };
}

Rgb fromOklab(const Lab& lab) {
    const float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    const float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    const float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;
    const Rgb c709 = {
        4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
    };
    return rec709ToRec2020(c709);
}

Rgb applySaturation(const Rgb& rgb, float amount) {
    if (std::abs(amount) < 1e-4f)
        return rgb;
    Lab lab = toOklab(rgb);
    const float scale = 1.0f + amount; // -1 -> greyscale, +1 -> double chroma
    lab.a *= scale;
    lab.b *= scale;
    return fromOklab(lab);
}

Rgb applyVibrance(const Rgb& rgb, float amount) {
    if (std::abs(amount) < 1e-4f)
        return rgb;
    Lab lab = toOklab(rgb);
    const float chroma = std::sqrt(lab.a * lab.a + lab.b * lab.b);
    const float weight = kVibranceHalf / (kVibranceHalf + chroma); // muted -> 1, vivid -> small
    const float scale = 1.0f + amount * weight;
    lab.a *= scale;
    lab.b *= scale;
    return fromOklab(lab);
}

float shoulderMap(float luminance, float amount) {
    if (amount <= 0.0f)
        return luminance; // exact identity, headroom included (docs/adr/0035)
    const float a = std::clamp(amount, 0.0f, 1.0f);
    const float knee = 1.0f - 0.5f * a; // a=1 -> 0.5; small a -> ~1 (only headroom rolled)
    if (luminance <= knee)
        return luminance;                               // shadows and midtones untouched
    const float x = (luminance - knee) / (1.0f - knee); // 0 at the knee, grows with headroom
    const float c = x / (1.0f + x);                     // slope 1 at the knee, -> 1 as x -> inf
    return knee + (1.0f - knee) * c;
}

Rgb applyFilmicHighlights(const Rgb& rgb, float amount) {
    if (amount <= 0.0f)
        return rgb;
    const float y = luma(rgb);
    if (y <= 1e-5f)
        return rgb;
    const float y2 = shoulderMap(y, amount);
    const float ratio = y2 / y; // <= 1 in highlights, 1 in shadows/midtones
    Rgb out = {rgb[0] * ratio, rgb[1] * ratio, rgb[2] * ratio}; // shoulder; hue preserved
    if (ratio > 0.999f)
        return out; // midtone: no compression, so no path-to-white work needed
    Lab lab = toOklab(out);
    const float chromaScale = std::pow(ratio, kPathToWhite); // fade chroma toward white
    lab.a *= chromaScale;
    lab.b *= chromaScale;
    return fromOklab(lab);
}

} // namespace colour
