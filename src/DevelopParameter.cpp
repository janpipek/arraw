#include "DevelopParameter.h"

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
    switch (p) {
    case DevelopParameter::Temperature: return "temperature";
    case DevelopParameter::Tint: return "tint";
    case DevelopParameter::Exposure: return "exposure";
    case DevelopParameter::Contrast: return "contrast";
    case DevelopParameter::Highlights: return "highlights";
    case DevelopParameter::Shadows: return "shadows";
    case DevelopParameter::Whites: return "whites";
    case DevelopParameter::Blacks: return "blacks";
    case DevelopParameter::Saturation: return "saturation";
    case DevelopParameter::Vibrance: return "vibrance";
    case DevelopParameter::CurveLuma: return "curveLuma";
    case DevelopParameter::CurveRed: return "curveRed";
    case DevelopParameter::CurveGreen: return "curveGreen";
    case DevelopParameter::CurveBlue: return "curveBlue";
    case DevelopParameter::Sharpening: return "sharpening";
    case DevelopParameter::ColorNoiseReduction: return "colorNoiseReduction";
    case DevelopParameter::ColorNoiseReductionSmoothness: return "colorNoiseReductionSmoothness";
    case DevelopParameter::LensDistortion: return "lensDistortion";
    case DevelopParameter::LensVignetting: return "lensVignetting";
    case DevelopParameter::LensChromaticAberration: return "lensChromaticAberration";
    case DevelopParameter::Orientation: return "orientation";
    case DevelopParameter::Straighten: return "straighten";
    case DevelopParameter::Crop: return "crop";
    case DevelopParameter::PostCropVignetteAmount: return "postCropVignetteAmount";
    case DevelopParameter::PostCropVignetteMidpoint: return "postCropVignetteMidpoint";
    case DevelopParameter::PostCropVignetteFeather: return "postCropVignetteFeather";
    case DevelopParameter::GrainAmount: return "grainAmount";
    case DevelopParameter::GrainSize: return "grainSize";
    case DevelopParameter::GrainRoughness: return "grainRoughness";
    default: return "";
    }
}

QString developParameterLabel(DevelopParameter p) {
    const auto trDev = [](const char* s) { return QCoreApplication::translate("DevelopParameter", s); };
    if (isHsl(p)) {
        const int off = hslOffset(p);
        return trDev(kHslBandLabels[off / 3]) + ' ' + trDev(kHslComponentLabels[off % 3]);
    }
    switch (p) {
    case DevelopParameter::Temperature: return trDev("Temperature");
    case DevelopParameter::Tint: return trDev("Tint");
    case DevelopParameter::Exposure: return trDev("Exposure");
    case DevelopParameter::Contrast: return trDev("Contrast");
    case DevelopParameter::Highlights: return trDev("Highlights");
    case DevelopParameter::Shadows: return trDev("Shadows");
    case DevelopParameter::Whites: return trDev("Whites");
    case DevelopParameter::Blacks: return trDev("Blacks");
    case DevelopParameter::Saturation: return trDev("Saturation");
    case DevelopParameter::Vibrance: return trDev("Vibrance");
    case DevelopParameter::CurveLuma: return trDev("Luma Curve");
    case DevelopParameter::CurveRed: return trDev("Red Curve");
    case DevelopParameter::CurveGreen: return trDev("Green Curve");
    case DevelopParameter::CurveBlue: return trDev("Blue Curve");
    case DevelopParameter::Sharpening: return trDev("Sharpening");
    case DevelopParameter::ColorNoiseReduction: return trDev("Color Noise Reduction");
    case DevelopParameter::ColorNoiseReductionSmoothness: return trDev("Color NR Smoothness");
    case DevelopParameter::LensDistortion: return trDev("Distortion Correction");
    case DevelopParameter::LensVignetting: return trDev("Vignetting Correction");
    case DevelopParameter::LensChromaticAberration: return trDev("CA Correction");
    case DevelopParameter::Orientation: return trDev("Orientation");
    case DevelopParameter::Straighten: return trDev("Straighten");
    case DevelopParameter::Crop: return trDev("Crop");
    case DevelopParameter::PostCropVignetteAmount: return trDev("Post-Crop Vignette Amount");
    case DevelopParameter::PostCropVignetteMidpoint: return trDev("Post-Crop Vignette Midpoint");
    case DevelopParameter::PostCropVignetteFeather: return trDev("Post-Crop Vignette Feather");
    case DevelopParameter::GrainAmount: return trDev("Grain Amount");
    case DevelopParameter::GrainSize: return trDev("Grain Size");
    case DevelopParameter::GrainRoughness: return trDev("Grain Roughness");
    default: return {};
    }
}

DevelopGroup developParameterGroup(DevelopParameter p) {
    if (isHsl(p))
        return DevelopGroup::Hsl;
    switch (p) {
    case DevelopParameter::Temperature:
    case DevelopParameter::Tint: return DevelopGroup::WhiteBalance;
    case DevelopParameter::Exposure:
    case DevelopParameter::Contrast:
    case DevelopParameter::Highlights:
    case DevelopParameter::Shadows:
    case DevelopParameter::Whites:
    case DevelopParameter::Blacks: return DevelopGroup::Tone;
    case DevelopParameter::Saturation:
    case DevelopParameter::Vibrance: return DevelopGroup::Colour;
    case DevelopParameter::CurveLuma:
    case DevelopParameter::CurveRed:
    case DevelopParameter::CurveGreen:
    case DevelopParameter::CurveBlue: return DevelopGroup::ToneCurve;
    case DevelopParameter::Sharpening:
    case DevelopParameter::ColorNoiseReduction:
    case DevelopParameter::ColorNoiseReductionSmoothness: return DevelopGroup::Detail;
    case DevelopParameter::LensDistortion:
    case DevelopParameter::LensVignetting:
    case DevelopParameter::LensChromaticAberration: return DevelopGroup::LensCorrections;
    case DevelopParameter::Orientation:
    case DevelopParameter::Straighten:
    case DevelopParameter::Crop: return DevelopGroup::Geometry;
    case DevelopParameter::PostCropVignetteAmount:
    case DevelopParameter::PostCropVignetteMidpoint:
    case DevelopParameter::PostCropVignetteFeather:
    case DevelopParameter::GrainAmount:
    case DevelopParameter::GrainSize:
    case DevelopParameter::GrainRoughness: return DevelopGroup::Effects;
    default: return DevelopGroup::Tone;
    }
}

bool developParameterDiffers(
    DevelopParameter p, const GlobalAdjustment& before, const GlobalAdjustment& after) {
    const GlobalAdjustment& a = before; // terse aliases for the long comparison switch
    const GlobalAdjustment& b = after;
    if (isHsl(p)) {
        const int off = hslOffset(p);
        const int band = off / 3;
        switch (off % 3) {
        case 0: return a.hslHue[band] != b.hslHue[band];
        case 1: return a.hslSat[band] != b.hslSat[band];
        default: return a.hslLum[band] != b.hslLum[band];
        }
    }
    switch (p) {
    case DevelopParameter::Temperature: return a.temperature != b.temperature;
    case DevelopParameter::Tint: return a.tint != b.tint;
    case DevelopParameter::Exposure: return a.exposure != b.exposure;
    case DevelopParameter::Contrast: return a.contrast != b.contrast;
    case DevelopParameter::Highlights: return a.highlights != b.highlights;
    case DevelopParameter::Shadows: return a.shadows != b.shadows;
    case DevelopParameter::Whites: return a.whites != b.whites;
    case DevelopParameter::Blacks: return a.blacks != b.blacks;
    case DevelopParameter::Saturation: return a.saturation != b.saturation;
    case DevelopParameter::Vibrance: return a.vibrance != b.vibrance;
    case DevelopParameter::CurveLuma: return a.curveLuma != b.curveLuma;
    case DevelopParameter::CurveRed: return a.curveR != b.curveR;
    case DevelopParameter::CurveGreen: return a.curveG != b.curveG;
    case DevelopParameter::CurveBlue: return a.curveB != b.curveB;
    case DevelopParameter::Sharpening: return a.sharpening != b.sharpening;
    case DevelopParameter::ColorNoiseReduction:
        return a.colorNoiseReduction != b.colorNoiseReduction;
    case DevelopParameter::ColorNoiseReductionSmoothness:
        return a.colorNoiseReductionSmoothness != b.colorNoiseReductionSmoothness;
    case DevelopParameter::LensDistortion: return a.lensCorrectDistortion != b.lensCorrectDistortion;
    case DevelopParameter::LensVignetting: return a.lensCorrectVignetting != b.lensCorrectVignetting;
    case DevelopParameter::LensChromaticAberration: return a.lensCorrectCA != b.lensCorrectCA;
    case DevelopParameter::Orientation: return !(a.orientation == b.orientation);
    case DevelopParameter::Straighten: return a.rotation != b.rotation;
    case DevelopParameter::Crop:
        return a.cropRect != b.cropRect || a.cropConstrained != b.cropConstrained;
    case DevelopParameter::PostCropVignetteAmount:
        return a.postCropVignetteAmount != b.postCropVignetteAmount;
    case DevelopParameter::PostCropVignetteMidpoint:
        return a.postCropVignetteMidpoint != b.postCropVignetteMidpoint;
    case DevelopParameter::PostCropVignetteFeather:
        return a.postCropVignetteFeather != b.postCropVignetteFeather;
    case DevelopParameter::GrainAmount: return a.grainAmount != b.grainAmount;
    case DevelopParameter::GrainSize: return a.grainSize != b.grainSize;
    case DevelopParameter::GrainRoughness: return a.grainRoughness != b.grainRoughness;
    default: return false;
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
    if (changed.size() == 1)
        return developParameterLabel(changed.front());

    // Several parameters moved at once (a reset, paste, preset, or WB pick). If
    // they share a group, name the group; otherwise fall back to the generic verb.
    const DevelopGroup first = developParameterGroup(changed.front());
    for (const DevelopParameter p : changed)
        if (developParameterGroup(p) != first)
            return QCoreApplication::translate("DevelopParameter", "Adjust");
    return developGroupLabel(first);
}
