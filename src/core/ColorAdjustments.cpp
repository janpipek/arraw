#include "ColorAdjustments.h"

#include "ColorSpaces.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

using namespace arraw;

namespace {

/// @brief Hue at the centre of each band, as a fraction of the wheel.
///
/// Red, orange, yellow, green, aqua, blue, purple, magenta. Mirrored by
/// `hslCenters` in `src/gpu/shaders/develop.frag`.
constexpr BandValues hslCenters{0.0F, 0.083F, 0.167F, 0.333F, 0.5F, 0.611F, 0.778F, 0.889F};

/// @brief Chroma at which Vibrance's weight is one half, in Oklab.
constexpr float vibranceHalf = 0.2F;

/// @brief Weight below which a band does not count toward a colour.
constexpr float negligibleWeight = 0.001F;

/// @brief Hue, saturation and value of a colour.
struct Hsv {
    float hue;        ///< Fraction of the wheel, zero up to one.
    float saturation; ///< Zero for grey to one.
    float value;      ///< The largest channel.
};

/// @brief Reads a colour as HSV.
Hsv toHsv(Colour c) {
    const float maxC = std::max(c[0], std::max(c[1], c[2]));
    const float minC = std::min(c[0], std::min(c[1], c[2]));
    const float delta = maxC - minC;
    float h = 0.0F;
    if (delta > 1e-5F) {
        if (maxC == c[0]) {
            h = (c[1] - c[2]) / delta + (c[1] < c[2] ? 6.0F : 0.0F);
        } else if (maxC == c[1]) {
            h = (c[2] - c[0]) / delta + 2.0F;
        } else {
            h = (c[0] - c[1]) / delta + 4.0F;
        }
        h /= 6.0F;
    }
    const float s = (maxC > 1e-5F) ? delta / maxC : 0.0F;
    return {h, s, maxC};
}

/// @brief Builds a colour from HSV; a hue outside the wheel is taken as the last sextant.
Colour fromHsv(Hsv hsv) {
    const float h = hsv.hue * 6.0F;
    const float s = hsv.saturation;
    const float v = hsv.value;
    const float whole = std::floor(h);
    const int i = (whole >= 0.0F && whole < 6.0F) ? static_cast<int>(whole) : 5;
    const float f = h - static_cast<float>(i);
    const float p = v * (1.0F - s);
    const float q = v * (1.0F - s * f);
    const float t = v * (1.0F - s * (1.0F - f));
    switch (i) {
    case 0:
        return {v, t, p};
    case 1:
        return {q, v, p};
    case 2:
        return {p, v, t};
    case 3:
        return {p, q, v};
    case 4:
        return {t, p, v};
    default:
        return {v, p, q};
    }
}

/// @brief Weight of one band for a hue: a smoothstep over sixty degrees, around the wheel.
float bandWeight(float hue, std::size_t band) {
    float d = std::abs(hue - hslCenters[band]);
    if (d > 0.5F) {
        d = 1.0F - d;
    }
    const float w = std::max(0.0F, 1.0F - d * 6.0F);
    return w * w * (3.0F - 2.0F * w);
}

/// @brief Values of one HSL control, from the bands in order.
template <float HueBand::* Member> BandValues hslValues(const HslSettings& hsl, const char* name) {
    const HueBand* bands[hueBandCount]{&hsl.red,  &hsl.orange, &hsl.yellow, &hsl.green,
                                       &hsl.aqua, &hsl.blue,   &hsl.purple, &hsl.magenta};
    BandValues values{};
    for (std::size_t i = 0; i < hueBandCount; ++i) {
        values[i] =
            clampedSetting(bands[i]->*Member, weakestHslControl, strongestHslControl, name) /
            strongestHslControl;
    }
    return values;
}

} // namespace

float arraw::clampedSetting(float value, float least, float most, const char* name) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string("A ") + name + " adjustment must be finite");
    }
    return std::clamp(value, least, most);
}

ColorAdjustmentPlan arraw::colorAdjustmentPlanFor(const ColorSettings& color,
                                                  const HslSettings& hsl,
                                                  const BlackAndWhiteSettings& blackAndWhite,
                                                  const ColorGradingSettings& colorGrading) {
    ColorAdjustmentPlan plan;
    const float saturation =
        clampedSetting(color.saturation, weakestSaturation, strongestSaturation, "saturation");
    const float vibrance =
        clampedSetting(color.vibrance, weakestSaturation, strongestSaturation, "vibrance");
    plan.adjustsSaturation = saturation != 0.0F;
    plan.saturation = saturation / strongestSaturation;
    plan.adjustsVibrance = vibrance != 0.0F;
    plan.vibrance = vibrance / strongestSaturation;

    plan.hueShift = hslValues<&HueBand::hue>(hsl, "hue");
    plan.bandSaturation = hslValues<&HueBand::saturation>(hsl, "band saturation");
    plan.bandLuminance = hslValues<&HueBand::luminance>(hsl, "band luminance");
    const auto any = [](const BandValues& values) {
        return std::ranges::any_of(values, [](float value) { return value != 0.0F; });
    };
    plan.adjustsHsl = any(plan.hueShift) || any(plan.bandSaturation) || any(plan.bandLuminance);

    plan.convertsToGrayscale = blackAndWhite.convertToGrayscale;
    const float mix[hueBandCount]{blackAndWhite.red,    blackAndWhite.orange, blackAndWhite.yellow,
                                  blackAndWhite.green,  blackAndWhite.aqua,   blackAndWhite.blue,
                                  blackAndWhite.purple, blackAndWhite.magenta};
    for (std::size_t i = 0; i < hueBandCount; ++i) {
        plan.grayMix[i] = clampedSetting(mix[i], darkestGrayMix, lightestGrayMix, "gray mix");
    }
    plan.grading = colorGradingPlanFor(colorGrading);
    return plan;
}

Oklab arraw::toOklab(Colour rgb) {
    const float r = 1.660491F * rgb[0] - 0.587641F * rgb[1] - 0.072850F * rgb[2];
    const float g = -0.124550F * rgb[0] + 1.132900F * rgb[1] - 0.008349F * rgb[2];
    const float bl = -0.018151F * rgb[0] - 0.100579F * rgb[1] + 1.118730F * rgb[2];
    const float l = 0.4122214708F * r + 0.5363325363F * g + 0.0514459929F * bl;
    const float m = 0.2119034982F * r + 0.6806995451F * g + 0.1073969566F * bl;
    const float s = 0.0883024619F * r + 0.2817188376F * g + 0.6299787005F * bl;
    const float l_ = std::cbrt(l);
    const float m_ = std::cbrt(m);
    const float s_ = std::cbrt(s);
    return {
        0.2104542553F * l_ + 0.7936177850F * m_ - 0.0040720468F * s_,
        1.9779984951F * l_ - 2.4285922050F * m_ + 0.4505937099F * s_,
        0.0259040371F * l_ + 0.7827717662F * m_ - 0.8086757660F * s_,
    };
}

Colour arraw::fromOklab(Oklab lab) {
    const float l_ = lab.lightness + 0.3963377774F * lab.a + 0.2158037573F * lab.b;
    const float m_ = lab.lightness - 0.1055613458F * lab.a - 0.0638541728F * lab.b;
    const float s_ = lab.lightness - 0.0894841775F * lab.a - 1.2914855480F * lab.b;
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;
    const float r = 4.0767416621F * l - 3.3077115913F * m + 0.2309699292F * s;
    const float g = -1.2684380046F * l + 2.6097574011F * m - 0.3413193965F * s;
    const float b = -0.0041960863F * l - 0.7034186147F * m + 1.7076147010F * s;
    return {0.627404F * r + 0.329283F * g + 0.043313F * b,
            0.069097F * r + 0.919541F * g + 0.011362F * b,
            0.016391F * r + 0.088013F * g + 0.895595F * b};
}

Colour arraw::applySaturation(Colour colour, float amount) {
    Oklab lab = toOklab(colour);
    const float scale = 1.0F + amount;
    lab.a *= scale;
    lab.b *= scale;
    return fromOklab(lab);
}

Colour arraw::applyVibrance(Colour colour, float amount) {
    Oklab lab = toOklab(colour);
    const float chroma = std::sqrt(lab.a * lab.a + lab.b * lab.b);
    const float weight = vibranceHalf / (vibranceHalf + chroma);
    const float scale = 1.0F + amount * weight;
    lab.a *= scale;
    lab.b *= scale;
    return fromOklab(lab);
}

Colour arraw::applyHsl(const ColorAdjustmentPlan& plan, Colour colour) {
    Hsv hsv = toHsv(colour);

    float totalHue = 0.0F;
    float totalSat = 0.0F;
    float totalLum = 0.0F;
    float totalW = 0.0F;
    for (std::size_t i = 0; i < hueBandCount; ++i) {
        const float w = bandWeight(hsv.hue, i);
        if (w > negligibleWeight) {
            totalHue += plan.hueShift[i] * w;
            totalSat += plan.bandSaturation[i] * w;
            totalLum += plan.bandLuminance[i] * w;
            totalW += w;
        }
    }
    if (totalW < negligibleWeight) {
        return colour;
    }

    const float wInv = 1.0F / totalW;
    const float turned = hsv.hue + totalHue * wInv / 12.0F; // a hundred is thirty degrees
    hsv.hue = turned - std::floor(turned);
    hsv.saturation = std::clamp(hsv.saturation * (1.0F + totalSat * wInv * 0.5F), 0.0F, 1.0F);
    // Not clamped above: headroom beyond white stays recoverable.
    hsv.value = std::max(hsv.value + totalLum * wInv * 0.5F, 0.0F);
    return fromHsv(hsv);
}

Colour arraw::applyBlackAndWhite(const BandValues& mix, Colour colour) {
    const float base = colorspaces::workingLuminance[0] * colour[0] +
                       colorspaces::workingLuminance[1] * colour[1] +
                       colorspaces::workingLuminance[2] * colour[2];
    const Hsv hsv = toHsv(colour);

    float weighted = 0.0F;
    float totalW = 0.0F;
    for (std::size_t i = 0; i < hueBandCount; ++i) {
        const float w = bandWeight(hsv.hue, i);
        if (w > negligibleWeight) {
            weighted += mix[i] * w;
            totalW += w;
        }
    }

    float gain = 1.0F;
    if (totalW > negligibleWeight) {
        const float blended = weighted / totalW;
        gain = 1.0F + (blended / 100.0F) * hsv.saturation;
    }
    const float grey = std::max(base * gain, 0.0F);
    return {grey, grey, grey};
}
