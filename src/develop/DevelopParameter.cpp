#include "develop/DevelopParameter.h"
#include "develop/DemosaicAlgorithm.h"
#include "develop/GlobalAdjustment.h"

#include <array>
#include <string>

#include <QCoreApplication>

namespace {

// HSL band labels in the order the GlobalAdjustment arrays use (singular, so a
// History line reads "Blue Hue" rather than "Blues Hue"). The plural forms in the
// AdjustmentPanel slider rows are a separate UI surface.
constexpr std::array<const char*, 8> kHslBandLabels
    = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
constexpr std::array<const char*, 8> kHslBandKeys
    = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
constexpr std::array<const char*, 3> kHslComponentLabels = {"Hue", "Saturation", "Luminance"};
constexpr std::array<const char*, 3> kHslComponentKeys = {"Hue", "Saturation", "Luminance"};

int hslOffset(DevelopParameter p) {
    return static_cast<int>(p) - static_cast<int>(DevelopParameter::HslRedHue);
}

bool isHsl(DevelopParameter p) {
    return p >= DevelopParameter::HslRedHue && p <= DevelopParameter::HslMagentaLuminance;
}

// The 8 Black & White mixer bands reuse the HSL band labels/keys; the band index
// is the offset from BwMixRed. The treatment toggle (BlackAndWhite) sits just
// before the block and is handled separately.
int bwMixOffset(DevelopParameter p) {
    return static_cast<int>(p) - static_cast<int>(DevelopParameter::BwMixRed);
}

bool isBwMix(DevelopParameter p) {
    return p >= DevelopParameter::BwMixRed && p <= DevelopParameter::BwMixMagenta;
}

} // namespace

const char* developParameterKey(DevelopParameter p) {
    if (isHsl(p)) {
        // Built once per call; the History panel doesn't query keys hot, and
        // granular presets serialise these rarely. Returns a stable thread_local.
        static thread_local std::string key;
        const int off = hslOffset(p);
        key = std::string("hsl") + kHslBandKeys[off / 3] + kHslComponentKeys[off % 3];
        return key.c_str();
    }
    if (isBwMix(p)) {
        static thread_local std::string key;
        key = std::string("bwMix") + kHslBandKeys[bwMixOffset(p)];
        return key.c_str();
    }
    switch (p) {
    case DevelopParameter::BlackAndWhite:
        return "blackAndWhite";
    case DevelopParameter::Temperature:
        return "temperature";
    case DevelopParameter::Tint:
        return "tint";
    case DevelopParameter::Exposure:
        return "exposure";
    case DevelopParameter::Contrast:
        return "contrast";
    case DevelopParameter::Highlights:
        return "highlights";
    case DevelopParameter::Shadows:
        return "shadows";
    case DevelopParameter::Whites:
        return "whites";
    case DevelopParameter::Blacks:
        return "blacks";
    case DevelopParameter::FilmicHighlights:
        return "filmicHighlights";
    case DevelopParameter::Saturation:
        return "saturation";
    case DevelopParameter::Vibrance:
        return "vibrance";
    case DevelopParameter::CurveLuma:
        return "curveLuma";
    case DevelopParameter::CurveRed:
        return "curveRed";
    case DevelopParameter::CurveGreen:
        return "curveGreen";
    case DevelopParameter::CurveBlue:
        return "curveBlue";
    case DevelopParameter::Demosaic:
        return "demosaic";
    case DevelopParameter::Texture:
        return "texture";
    case DevelopParameter::Clarity:
        return "clarity";
    case DevelopParameter::Dehaze:
        return "dehaze";
    case DevelopParameter::Sharpening:
        return "sharpening";
    case DevelopParameter::ColorNoiseReduction:
        return "colorNoiseReduction";
    case DevelopParameter::ColorNoiseReductionSmoothness:
        return "colorNoiseReductionSmoothness";
    case DevelopParameter::LensDistortion:
        return "lensDistortion";
    case DevelopParameter::LensVignetting:
        return "lensVignetting";
    case DevelopParameter::LensChromaticAberration:
        return "lensChromaticAberration";
    case DevelopParameter::Orientation:
        return "orientation";
    case DevelopParameter::Straighten:
        return "straighten";
    case DevelopParameter::Crop:
        return "crop";
    case DevelopParameter::PostCropVignetteAmount:
        return "postCropVignetteAmount";
    case DevelopParameter::PostCropVignetteMidpoint:
        return "postCropVignetteMidpoint";
    case DevelopParameter::PostCropVignetteFeather:
        return "postCropVignetteFeather";
    case DevelopParameter::GrainAmount:
        return "grainAmount";
    case DevelopParameter::GrainSize:
        return "grainSize";
    case DevelopParameter::GrainRoughness:
        return "grainRoughness";
    default:
        return "";
    }
}

QString developParameterLabel(DevelopParameter p) {
    const auto trDev = [](const char* s) {
        return QCoreApplication::translate("DevelopParameter", s);
    };
    if (isHsl(p)) {
        const int off = hslOffset(p);
        return trDev(kHslBandLabels[off / 3]) + ' ' + trDev(kHslComponentLabels[off % 3]);
    }
    if (isBwMix(p)) // e.g. "B&W Blue" — the mixer's Blue band
        return trDev("B&W") + ' ' + trDev(kHslBandLabels[bwMixOffset(p)]);
    switch (p) {
    case DevelopParameter::BlackAndWhite:
        return trDev("Black & White");
    case DevelopParameter::Temperature:
        return trDev("Temperature");
    case DevelopParameter::Tint:
        return trDev("Tint");
    case DevelopParameter::Exposure:
        return trDev("Exposure");
    case DevelopParameter::Contrast:
        return trDev("Contrast");
    case DevelopParameter::Highlights:
        return trDev("Highlights");
    case DevelopParameter::Shadows:
        return trDev("Shadows");
    case DevelopParameter::Whites:
        return trDev("Whites");
    case DevelopParameter::Blacks:
        return trDev("Blacks");
    case DevelopParameter::FilmicHighlights:
        return trDev("Filmic Highlights");
    case DevelopParameter::Saturation:
        return trDev("Saturation");
    case DevelopParameter::Vibrance:
        return trDev("Vibrance");
    case DevelopParameter::CurveLuma:
        return trDev("Luma Curve");
    case DevelopParameter::CurveRed:
        return trDev("Red Curve");
    case DevelopParameter::CurveGreen:
        return trDev("Green Curve");
    case DevelopParameter::CurveBlue:
        return trDev("Blue Curve");
    case DevelopParameter::Demosaic:
        return trDev("Demosaic");
    case DevelopParameter::Texture:
        return trDev("Texture");
    case DevelopParameter::Clarity:
        return trDev("Clarity");
    case DevelopParameter::Dehaze:
        return trDev("Dehaze");
    case DevelopParameter::Sharpening:
        return trDev("Sharpening");
    case DevelopParameter::ColorNoiseReduction:
        return trDev("Color Noise Reduction");
    case DevelopParameter::ColorNoiseReductionSmoothness:
        return trDev("Color NR Smoothness");
    case DevelopParameter::LensDistortion:
        return trDev("Distortion Correction");
    case DevelopParameter::LensVignetting:
        return trDev("Vignetting Correction");
    case DevelopParameter::LensChromaticAberration:
        return trDev("CA Correction");
    case DevelopParameter::Orientation:
        return trDev("Orientation");
    case DevelopParameter::Straighten:
        return trDev("Straighten");
    case DevelopParameter::Crop:
        return trDev("Crop");
    case DevelopParameter::PostCropVignetteAmount:
        return trDev("Post-Crop Vignette Amount");
    case DevelopParameter::PostCropVignetteMidpoint:
        return trDev("Post-Crop Vignette Midpoint");
    case DevelopParameter::PostCropVignetteFeather:
        return trDev("Post-Crop Vignette Feather");
    case DevelopParameter::GrainAmount:
        return trDev("Grain Amount");
    case DevelopParameter::GrainSize:
        return trDev("Grain Size");
    case DevelopParameter::GrainRoughness:
        return trDev("Grain Roughness");
    default:
        return {};
    }
}

DevelopGroup developParameterGroup(DevelopParameter p) {
    if (isHsl(p))
        return DevelopGroup::Hsl;
    if (isBwMix(p))
        return DevelopGroup::BlackAndWhite;
    switch (p) {
    case DevelopParameter::BlackAndWhite:
        return DevelopGroup::BlackAndWhite;
    case DevelopParameter::Temperature:
    case DevelopParameter::Tint:
        return DevelopGroup::WhiteBalance;
    case DevelopParameter::Exposure:
    case DevelopParameter::Contrast:
    case DevelopParameter::Highlights:
    case DevelopParameter::Shadows:
    case DevelopParameter::Whites:
    case DevelopParameter::Blacks:
    case DevelopParameter::FilmicHighlights:
        return DevelopGroup::Tone;
    case DevelopParameter::Saturation:
    case DevelopParameter::Vibrance:
        return DevelopGroup::Colour;
    case DevelopParameter::CurveLuma:
    case DevelopParameter::CurveRed:
    case DevelopParameter::CurveGreen:
    case DevelopParameter::CurveBlue:
        return DevelopGroup::ToneCurve;
    case DevelopParameter::Demosaic:
    case DevelopParameter::Texture:
    case DevelopParameter::Clarity:
    case DevelopParameter::Dehaze:
    case DevelopParameter::Sharpening:
    case DevelopParameter::ColorNoiseReduction:
    case DevelopParameter::ColorNoiseReductionSmoothness:
        return DevelopGroup::Detail;
    case DevelopParameter::LensDistortion:
    case DevelopParameter::LensVignetting:
    case DevelopParameter::LensChromaticAberration:
        return DevelopGroup::LensCorrections;
    case DevelopParameter::Orientation:
    case DevelopParameter::Straighten:
    case DevelopParameter::Crop:
        return DevelopGroup::Geometry;
    case DevelopParameter::PostCropVignetteAmount:
    case DevelopParameter::PostCropVignetteMidpoint:
    case DevelopParameter::PostCropVignetteFeather:
    case DevelopParameter::GrainAmount:
    case DevelopParameter::GrainSize:
    case DevelopParameter::GrainRoughness:
        return DevelopGroup::Effects;
    default:
        return DevelopGroup::Tone;
    }
}

std::optional<FieldSpec> developParameterSpec(DevelopParameter p) {
    // Three specs cover most sliders; the named locals match AdjustmentPanel's
    // former constants (now sourced from here, so the numbers live in one place).
    const FieldSpec bipolar{-100, 100, 0, 1.0f, 1.0f, 0, {}, true, 1.0f};
    const FieldSpec unipolar0{0, 100, 0, 1.0f, 1.0f, 0, {}, false, 1.0f};
    const FieldSpec unipolar25{0, 100, 25, 1.0f, 1.0f, 0, {}, false, 1.0f};
    const FieldSpec unipolar50{0, 100, 50, 1.0f, 1.0f, 0, {}, false, 1.0f};
    const QString degrees = QString::fromUtf8("\xc2\xb0");
    if (isHsl(p)) // Hue reads in degrees (±30°); Saturation/Luminance are bipolar
        return hslOffset(p) % 3 == 0 ? FieldSpec{-100, 100, 0, 1.0f, 0.3f, 1, degrees, true, 0.3f}
                                     : bipolar;
    if (isBwMix(p)) // each mixer band is a bipolar -100..+100 weight
        return bipolar;
    switch (p) {
    case DevelopParameter::Temperature:
        return FieldSpec{2000, 12000, 5500, 1.0f, 1.0f, 0, " K", false, 50.0f};
    case DevelopParameter::Exposure:
        return FieldSpec{-500, 500, 0, 0.01f, 0.01f, 2, " EV", true, 0.05f};
    case DevelopParameter::Tint:
    case DevelopParameter::Contrast:
    case DevelopParameter::Highlights:
    case DevelopParameter::Shadows:
    case DevelopParameter::Whites:
    case DevelopParameter::Blacks:
    case DevelopParameter::Saturation:
    case DevelopParameter::Vibrance:
    case DevelopParameter::Texture:
    case DevelopParameter::Clarity:
    case DevelopParameter::Dehaze:
    case DevelopParameter::PostCropVignetteAmount:
        return bipolar;
    case DevelopParameter::Sharpening:
    case DevelopParameter::ColorNoiseReduction:
    case DevelopParameter::GrainAmount:
        return unipolar0;
    case DevelopParameter::FilmicHighlights:
        return unipolar25;
    case DevelopParameter::ColorNoiseReductionSmoothness:
    case DevelopParameter::PostCropVignetteMidpoint:
    case DevelopParameter::PostCropVignetteFeather:
    case DevelopParameter::GrainSize:
    case DevelopParameter::GrainRoughness:
        return unipolar50;
    case DevelopParameter::Straighten:
        return FieldSpec{-4500, 4500, 0, 0.01f, 0.01f, 2, degrees, true, 0.10f};
    default:
        return std::nullopt; // curves, crop, orientation, lens toggles
    }
}

float developParameterValue(DevelopParameter p, const GlobalAdjustment& s) {
    if (isHsl(p)) {
        const int off = hslOffset(p);
        const int band = off / 3;
        switch (off % 3) {
        case 0:
            return s.hslHue[band];
        case 1:
            return s.hslSat[band];
        default:
            return s.hslLum[band];
        }
    }
    if (isBwMix(p))
        return s.bwMix[bwMixOffset(p)];
    switch (p) {
    case DevelopParameter::Temperature:
        return s.temperature;
    case DevelopParameter::Tint:
        return s.tint;
    case DevelopParameter::Exposure:
        return s.exposure;
    case DevelopParameter::Contrast:
        return s.contrast;
    case DevelopParameter::Highlights:
        return s.highlights;
    case DevelopParameter::Shadows:
        return s.shadows;
    case DevelopParameter::Whites:
        return s.whites;
    case DevelopParameter::Blacks:
        return s.blacks;
    case DevelopParameter::FilmicHighlights:
        return s.filmicHighlights;
    case DevelopParameter::Saturation:
        return s.saturation;
    case DevelopParameter::Vibrance:
        return s.vibrance;
    case DevelopParameter::Texture:
        return s.texture;
    case DevelopParameter::Clarity:
        return s.clarity;
    case DevelopParameter::Dehaze:
        return s.dehaze;
    case DevelopParameter::Sharpening:
        return s.sharpening;
    case DevelopParameter::ColorNoiseReduction:
        return s.colorNoiseReduction;
    case DevelopParameter::ColorNoiseReductionSmoothness:
        return s.colorNoiseReductionSmoothness;
    case DevelopParameter::Straighten:
        return s.rotation;
    case DevelopParameter::PostCropVignetteAmount:
        return s.postCropVignetteAmount;
    case DevelopParameter::PostCropVignetteMidpoint:
        return s.postCropVignetteMidpoint;
    case DevelopParameter::PostCropVignetteFeather:
        return s.postCropVignetteFeather;
    case DevelopParameter::GrainAmount:
        return s.grainAmount;
    case DevelopParameter::GrainSize:
        return s.grainSize;
    case DevelopParameter::GrainRoughness:
        return s.grainRoughness;
    default:
        return 0.0f; // curves, crop, orientation, lens toggles
    }
}

QString developParameterValueText(DevelopParameter p, const GlobalAdjustment& s) {
    const auto onOff = [](bool on) {
        return on ? QCoreApplication::translate("DevelopParameter", "on")
                  : QCoreApplication::translate("DevelopParameter", "off");
    };
    switch (p) {
    case DevelopParameter::Demosaic:
        return demosaicToken(s.demosaicAlgorithm);
    case DevelopParameter::BlackAndWhite:
        return onOff(s.convertToGrayscale);
    case DevelopParameter::LensDistortion:
        return onOff(s.lensCorrectDistortion);
    case DevelopParameter::LensVignetting:
        return onOff(s.lensCorrectVignetting);
    case DevelopParameter::LensChromaticAberration:
        return onOff(s.lensCorrectCA);
    default:
        break;
    }
    const std::optional<FieldSpec> spec = developParameterSpec(p);
    if (!spec)
        return {}; // curves, crop, orientation — no single value to show
    return spec->format(spec->fromParam(developParameterValue(p, s)));
}

bool developParameterDiffers(
    DevelopParameter p, const GlobalAdjustment& before, const GlobalAdjustment& after) {
    const GlobalAdjustment& a = before; // terse aliases for the comparison switch
    const GlobalAdjustment& b = after;
    switch (p) {
    case DevelopParameter::Demosaic:
        return a.demosaicAlgorithm != b.demosaicAlgorithm;
    case DevelopParameter::BlackAndWhite:
        return a.convertToGrayscale != b.convertToGrayscale;
    case DevelopParameter::CurveLuma:
        return a.curveLuma != b.curveLuma;
    case DevelopParameter::CurveRed:
        return a.curveR != b.curveR;
    case DevelopParameter::CurveGreen:
        return a.curveG != b.curveG;
    case DevelopParameter::CurveBlue:
        return a.curveB != b.curveB;
    case DevelopParameter::LensDistortion:
        return a.lensCorrectDistortion != b.lensCorrectDistortion;
    case DevelopParameter::LensVignetting:
        return a.lensCorrectVignetting != b.lensCorrectVignetting;
    case DevelopParameter::LensChromaticAberration:
        return a.lensCorrectCA != b.lensCorrectCA;
    case DevelopParameter::Orientation:
        return !(a.orientation == b.orientation);
    case DevelopParameter::Crop:
        return a.cropRect != b.cropRect || a.cropConstrained != b.cropConstrained;
    // Every scalar parameter (incl. HSL) is its single backing value.
    default:
        return developParameterValue(p, a) != developParameterValue(p, b);
    }
}

std::vector<DevelopParameter> changedParameters(
    const GlobalAdjustment& before, const GlobalAdjustment& after) {
    std::vector<DevelopParameter> changed;
    for (int i = 0; i < kDevelopParameterCount; ++i) {
        const auto p = static_cast<DevelopParameter>(i);
        if (developParameterDiffers(p, before, after))
            changed.push_back(p);
    }
    return changed;
}

QString developChangeLabel(const GlobalAdjustment& before, const GlobalAdjustment& after) {
    const std::vector<DevelopParameter> changed = changedParameters(before, after);
    if (changed.empty())
        return QCoreApplication::translate("DevelopParameter", "Adjust");
    if (changed.size() == 1) {
        const DevelopParameter p = changed.front();
        const QString label = developParameterLabel(p);
        const QString value = developParameterValueText(p, after);
        return value.isEmpty() ? label : label + ' ' + value;
    }

    // Several parameters moved at once (a reset, paste, preset, or WB pick). If
    // they share a group, name the group; otherwise fall back to the generic verb.
    const DevelopGroup first = developParameterGroup(changed.front());
    for (const DevelopParameter p : changed)
        if (developParameterGroup(p) != first)
            return QCoreApplication::translate("DevelopParameter", "Adjust");
    return developGroupLabel(first);
}
