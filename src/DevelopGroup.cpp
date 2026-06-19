#include "DevelopGroup.h"

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
    case DevelopGroup::Detail:
        return "detail";
    case DevelopGroup::Geometry:
        return "geometry";
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
    case DevelopGroup::Detail:
        return QObject::tr("Detail");
    case DevelopGroup::Geometry:
        return QObject::tr("Geometry");
    case DevelopGroup::Count_:
        break;
    }
    return {};
}

// Each group overwrites exactly its own fields on `result` from `source`. The
// groups partition every global field of GlobalAdjustment; localAdjustments is
// intentionally absent from every arm, so it always rides through from `target`
// (docs/adr/0023, CONTEXT.md "Develop Group").
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
    if (hasGroup(selection, DevelopGroup::Detail)) {
        result.sharpening = source.sharpening;
    }
    if (hasGroup(selection, DevelopGroup::Geometry)) {
        result.rotation = source.rotation;
        result.cropRect = source.cropRect;
        result.cropConstrained = source.cropConstrained;
    }

    return result;
}
