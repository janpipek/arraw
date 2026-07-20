#include "develop/DevelopGroup.h"
#include "develop/GlobalAdjustment.h"

#include <QObject>

const char* developGroupKey(DevelopGroup g) {
    switch (g) {
    case DevelopGroup::WhiteBalance:
        return "whiteBalance";
    case DevelopGroup::Tone:
        return "tone";
    case DevelopGroup::ToneCurve:
        return "toneCurve";
    case DevelopGroup::Colour:
        return "colour";
    case DevelopGroup::Hsl:
        return "hsl";
    case DevelopGroup::BlackAndWhite:
        return "blackAndWhite";
    case DevelopGroup::Detail:
        return "detail";
    case DevelopGroup::Geometry:
        return "geometry";
    case DevelopGroup::LensCorrections:
        return "lensCorrections";
    case DevelopGroup::Effects:
        return "effects";
    case DevelopGroup::Count_:
        break;
    }
    return "";
}

QString developGroupLabel(DevelopGroup g) {
    switch (g) {
    case DevelopGroup::WhiteBalance:
        return QObject::tr("White Balance");
    case DevelopGroup::Tone:
        return QObject::tr("Tone");
    case DevelopGroup::ToneCurve:
        return QObject::tr("Tone Curve");
    case DevelopGroup::Colour:
        return QObject::tr("Colour");
    case DevelopGroup::Hsl:
        return QObject::tr("HSL");
    case DevelopGroup::BlackAndWhite:
        return QObject::tr("Black & White");
    case DevelopGroup::Detail:
        return QObject::tr("Detail");
    case DevelopGroup::Geometry:
        return QObject::tr("Geometry");
    case DevelopGroup::LensCorrections:
        return QObject::tr("Lens Corrections");
    case DevelopGroup::Effects:
        return QObject::tr("Effects");
    case DevelopGroup::Count_:
        break;
    }
    return {};
}

// Each group overwrites exactly its visible controls on `result` from `source`.
// Per-image state (localAdjustments, spots, Grain seed) is intentionally absent,
// so it always rides through from `target` (docs/adr/0023, docs/adr/0026).
GlobalAdjustment applyGroups(
    const GlobalAdjustment& target, const GlobalAdjustment& source, GroupSelection selection) {
    GlobalAdjustment result = target;

    if (hasGroup(selection, DevelopGroup::WhiteBalance)) {
        result.temperature = source.temperature;
        result.tint = source.tint;
    }
    if (hasGroup(selection, DevelopGroup::Tone)) {
        result.exposure = source.exposure;
        result.contrast = source.contrast;
        result.highlights = source.highlights;
        result.shadows = source.shadows;
        result.whites = source.whites;
        result.blacks = source.blacks;
        result.filmicHighlights = source.filmicHighlights;
    }
    if (hasGroup(selection, DevelopGroup::ToneCurve)) {
        result.curveLuma = source.curveLuma;
        result.curveR = source.curveR;
        result.curveG = source.curveG;
        result.curveB = source.curveB;
    }
    if (hasGroup(selection, DevelopGroup::Colour)) {
        result.saturation = source.saturation;
        result.vibrance = source.vibrance;
    }
    if (hasGroup(selection, DevelopGroup::Hsl)) {
        result.hslHue = source.hslHue;
        result.hslSat = source.hslSat;
        result.hslLum = source.hslLum;
    }
    if (hasGroup(selection, DevelopGroup::BlackAndWhite)) {
        result.convertToGrayscale = source.convertToGrayscale; // toggle + mix travel as one unit
        result.bwMix = source.bwMix;
    }
    if (hasGroup(selection, DevelopGroup::Detail)) {
        result.demosaicAlgorithm = source.demosaicAlgorithm; // re-decodes on apply (issue #22)
        result.texture = source.texture;
        result.clarity = source.clarity;
        result.dehaze = source.dehaze;
        result.sharpening = source.sharpening;
        result.colorNoiseReduction = source.colorNoiseReduction; // Strength (issue #59)
        result.colorNoiseReductionSmoothness = source.colorNoiseReductionSmoothness;
        result.luminanceNoiseReduction = source.luminanceNoiseReduction; // Amount (docs/adr/0046)
        result.luminanceNoiseReductionDetail = source.luminanceNoiseReductionDetail;
    }
    if (hasGroup(selection, DevelopGroup::Geometry)) {
        result.orientation = source.orientation;
        result.rotation = source.rotation;
        result.cropRect = source.cropRect;
        result.cropConstrained = source.cropConstrained;
    }
    if (hasGroup(selection, DevelopGroup::LensCorrections)) {
        result.lensCorrectDistortion = source.lensCorrectDistortion;
        result.lensCorrectVignetting = source.lensCorrectVignetting;
        result.lensCorrectCA = source.lensCorrectCA;
    }
    if (hasGroup(selection, DevelopGroup::Effects)) {
        result.postCropVignetteAmount = source.postCropVignetteAmount;
        result.postCropVignetteMidpoint = source.postCropVignetteMidpoint;
        result.postCropVignetteFeather = source.postCropVignetteFeather;
        result.grainAmount = source.grainAmount;
        result.grainSize = source.grainSize;
        result.grainRoughness = source.grainRoughness;
    }

    return result;
}

namespace {

// Whether any field `g` carries in `v` differs from GlobalAdjustment{}'s default —
// exact equality for scalars/bools/arrays, CurvePoints::isIdentity() (its existing
// tolerance) for the four tone curves (docs/adr/0049).
bool groupDiffersFromDefault(DevelopGroup g, const GlobalAdjustment& v) {
    static const GlobalAdjustment kDefault;
    switch (g) {
    case DevelopGroup::WhiteBalance:
        return v.temperature != kDefault.temperature || v.tint != kDefault.tint;
    case DevelopGroup::Tone:
        return v.exposure != kDefault.exposure || v.contrast != kDefault.contrast
               || v.highlights != kDefault.highlights || v.shadows != kDefault.shadows
               || v.whites != kDefault.whites || v.blacks != kDefault.blacks
               || v.filmicHighlights != kDefault.filmicHighlights;
    case DevelopGroup::ToneCurve:
        return !v.curveLuma.isIdentity() || !v.curveR.isIdentity() || !v.curveG.isIdentity()
               || !v.curveB.isIdentity();
    case DevelopGroup::Colour:
        return v.saturation != kDefault.saturation || v.vibrance != kDefault.vibrance;
    case DevelopGroup::Hsl:
        return v.hslHue != kDefault.hslHue || v.hslSat != kDefault.hslSat
               || v.hslLum != kDefault.hslLum;
    case DevelopGroup::BlackAndWhite:
        return v.convertToGrayscale != kDefault.convertToGrayscale || v.bwMix != kDefault.bwMix;
    case DevelopGroup::Detail:
        return v.demosaicAlgorithm != kDefault.demosaicAlgorithm
               || v.texture != kDefault.texture || v.clarity != kDefault.clarity
               || v.dehaze != kDefault.dehaze || v.sharpening != kDefault.sharpening
               || v.colorNoiseReduction != kDefault.colorNoiseReduction
               || v.colorNoiseReductionSmoothness != kDefault.colorNoiseReductionSmoothness
               || v.luminanceNoiseReduction != kDefault.luminanceNoiseReduction
               || v.luminanceNoiseReductionDetail != kDefault.luminanceNoiseReductionDetail;
    case DevelopGroup::Geometry:
        return v.orientation != kDefault.orientation || v.rotation != kDefault.rotation
               || v.cropRect != kDefault.cropRect || v.cropConstrained != kDefault.cropConstrained;
    case DevelopGroup::LensCorrections:
        return v.lensCorrectDistortion != kDefault.lensCorrectDistortion
               || v.lensCorrectVignetting != kDefault.lensCorrectVignetting
               || v.lensCorrectCA != kDefault.lensCorrectCA;
    case DevelopGroup::Effects:
        return v.postCropVignetteAmount != kDefault.postCropVignetteAmount
               || v.postCropVignetteMidpoint != kDefault.postCropVignetteMidpoint
               || v.postCropVignetteFeather != kDefault.postCropVignetteFeather
               || v.grainAmount != kDefault.grainAmount || v.grainSize != kDefault.grainSize
               || v.grainRoughness != kDefault.grainRoughness;
    case DevelopGroup::Count_:
        break;
    }
    return false;
}

} // namespace

GroupSelection groupsWithNonDefaultValues(const GlobalAdjustment& v) {
    GroupSelection sel;
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        if (groupDiffersFromDefault(g, v))
            sel.set(static_cast<size_t>(i));
    }
    return sel;
}
