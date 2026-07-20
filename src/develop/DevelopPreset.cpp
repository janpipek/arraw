#include "develop/DevelopPreset.h"
#include "core/Orientation.h"
#include "develop/GlobalAdjustment.h"

#include <array>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QJsonArray curveToJson(const CurvePoints& c) {
    QJsonArray arr;
    for (const QPointF& p : c.points)
        arr.append(QJsonArray{p.x(), p.y()});
    return arr;
}

CurvePoints curveFromJson(const QJsonArray& arr) {
    CurvePoints c;
    c.points.clear();
    for (const auto& v : arr) {
        const QJsonArray pt = v.toArray();
        c.points.emplace_back(pt.at(0).toDouble(), pt.at(1).toDouble());
    }
    return c;
}

QJsonArray bandsToJson(const std::array<float, 8>& bands) {
    QJsonArray arr;
    for (float b : bands)
        arr.append(b);
    return arr;
}

void bandsFromJson(const QJsonArray& arr, std::array<float, 8>& bands) {
    for (int i = 0; i < 8 && i < arr.size(); ++i)
        bands[i] = static_cast<float>(arr.at(i).toDouble());
}

// Writes one group's fields from `v` into a fresh JSON object.
QJsonObject groupToJson(DevelopGroup g, const GlobalAdjustment& v) {
    QJsonObject o;
    switch (g) {
    case DevelopGroup::WhiteBalance:
        o["temperature"] = v.temperature;
        o["tint"] = v.tint;
        break;
    case DevelopGroup::Tone:
        o["exposure"] = v.exposure;
        o["contrast"] = v.contrast;
        o["highlights"] = v.highlights;
        o["shadows"] = v.shadows;
        o["whites"] = v.whites;
        o["blacks"] = v.blacks;
        o["filmicHighlights"] = v.filmicHighlights;
        break;
    case DevelopGroup::ToneCurve:
        o["luma"] = curveToJson(v.curveLuma);
        o["r"] = curveToJson(v.curveR);
        o["g"] = curveToJson(v.curveG);
        o["b"] = curveToJson(v.curveB);
        break;
    case DevelopGroup::Colour:
        o["saturation"] = v.saturation;
        o["vibrance"] = v.vibrance;
        break;
    case DevelopGroup::Hsl:
        o["hue"] = bandsToJson(v.hslHue);
        o["sat"] = bandsToJson(v.hslSat);
        o["lum"] = bandsToJson(v.hslLum);
        break;
    case DevelopGroup::BlackAndWhite:
        o["convertToGrayscale"] = v.convertToGrayscale;
        o["mix"] = bandsToJson(v.bwMix);
        break;
    case DevelopGroup::Detail:
        o["texture"] = v.texture;
        o["clarity"] = v.clarity;
        o["dehaze"] = v.dehaze;
        o["sharpening"] = v.sharpening;
        o["colorNoiseReduction"] = v.colorNoiseReduction; // Strength (issue #59)
        o["colorNoiseReductionSmoothness"] = v.colorNoiseReductionSmoothness;
        o["luminanceNoiseReduction"] = v.luminanceNoiseReduction; // Amount (docs/adr/0046)
        o["luminanceNoiseReductionDetail"] = v.luminanceNoiseReductionDetail;
        break;
    case DevelopGroup::Geometry:
        o["orientation"] = orient::toExif(v.orientation); // EXIF 1..8 (docs/adr/0029)
        o["rotation"] = v.rotation;
        o["crop"]
            = QJsonArray{v.cropRect.x(), v.cropRect.y(), v.cropRect.width(), v.cropRect.height()};
        o["constrained"] = v.cropConstrained;
        break;
    case DevelopGroup::LensCorrections:
        o["distortion"] = v.lensCorrectDistortion;
        o["vignetting"] = v.lensCorrectVignetting;
        o["ca"] = v.lensCorrectCA;
        break;
    case DevelopGroup::Effects:
        o["postCropVignetteAmount"] = v.postCropVignetteAmount;
        o["postCropVignetteMidpoint"] = v.postCropVignetteMidpoint;
        o["postCropVignetteFeather"] = v.postCropVignetteFeather;
        o["grainAmount"] = v.grainAmount;
        o["grainSize"] = v.grainSize;
        o["grainRoughness"] = v.grainRoughness;
        break;
    case DevelopGroup::Count_:
        break;
    }
    return o;
}

// Reads one group's fields from `o` into `v` (leaving absent fields untouched).
void groupFromJson(DevelopGroup g, const QJsonObject& o, GlobalAdjustment& v) {
    const auto f = [&](const char* key, float fallback) {
        return static_cast<float>(o.value(key).toDouble(fallback));
    };
    switch (g) {
    case DevelopGroup::WhiteBalance:
        v.temperature = f("temperature", v.temperature);
        v.tint = f("tint", v.tint);
        break;
    case DevelopGroup::Tone:
        v.exposure = f("exposure", v.exposure);
        v.contrast = f("contrast", v.contrast);
        v.highlights = f("highlights", v.highlights);
        v.shadows = f("shadows", v.shadows);
        v.whites = f("whites", v.whites);
        v.blacks = f("blacks", v.blacks);
        v.filmicHighlights = f("filmicHighlights", v.filmicHighlights);
        break;
    case DevelopGroup::ToneCurve:
        v.curveLuma = curveFromJson(o["luma"].toArray());
        v.curveR = curveFromJson(o["r"].toArray());
        v.curveG = curveFromJson(o["g"].toArray());
        v.curveB = curveFromJson(o["b"].toArray());
        break;
    case DevelopGroup::Colour:
        v.saturation = f("saturation", v.saturation);
        v.vibrance = f("vibrance", v.vibrance);
        break;
    case DevelopGroup::Hsl:
        bandsFromJson(o["hue"].toArray(), v.hslHue);
        bandsFromJson(o["sat"].toArray(), v.hslSat);
        bandsFromJson(o["lum"].toArray(), v.hslLum);
        break;
    case DevelopGroup::BlackAndWhite:
        v.convertToGrayscale = o.value("convertToGrayscale").toBool(v.convertToGrayscale);
        bandsFromJson(o["mix"].toArray(), v.bwMix);
        break;
    case DevelopGroup::Detail:
        v.texture = f("texture", v.texture);
        v.clarity = f("clarity", v.clarity);
        v.dehaze = f("dehaze", v.dehaze);
        v.sharpening = f("sharpening", v.sharpening);
        v.colorNoiseReduction = f("colorNoiseReduction", v.colorNoiseReduction); // Strength
        v.colorNoiseReductionSmoothness
            = f("colorNoiseReductionSmoothness", v.colorNoiseReductionSmoothness);
        v.luminanceNoiseReduction = f("luminanceNoiseReduction", v.luminanceNoiseReduction); // Amt
        v.luminanceNoiseReductionDetail
            = f("luminanceNoiseReductionDetail", v.luminanceNoiseReductionDetail);
        break;
    case DevelopGroup::Geometry: {
        if (o.contains("orientation"))
            v.orientation = orient::fromExif(o.value("orientation").toInt(1));
        v.rotation = f("rotation", v.rotation);
        const QJsonArray c = o["crop"].toArray();
        if (c.size() == 4)
            v.cropRect = QRectF(
                c.at(0).toDouble(), c.at(1).toDouble(), c.at(2).toDouble(), c.at(3).toDouble());
        v.cropConstrained = o.value("constrained").toBool(v.cropConstrained);
        break;
    }
    case DevelopGroup::LensCorrections:
        v.lensCorrectDistortion = o.value("distortion").toBool(v.lensCorrectDistortion);
        v.lensCorrectVignetting = o.value("vignetting").toBool(v.lensCorrectVignetting);
        v.lensCorrectCA = o.value("ca").toBool(v.lensCorrectCA);
        break;
    case DevelopGroup::Effects:
        v.postCropVignetteAmount = f("postCropVignetteAmount", v.postCropVignetteAmount);
        v.postCropVignetteMidpoint = f("postCropVignetteMidpoint", v.postCropVignetteMidpoint);
        v.postCropVignetteFeather = f("postCropVignetteFeather", v.postCropVignetteFeather);
        v.grainAmount = f("grainAmount", v.grainAmount);
        v.grainSize = f("grainSize", v.grainSize);
        v.grainRoughness = f("grainRoughness", v.grainRoughness);
        break;
    case DevelopGroup::Count_:
        break;
    }
}

} // namespace

QByteArray serializeDevelopPreset(const DevelopPreset& preset) {
    QJsonObject groups;
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const auto g = static_cast<DevelopGroup>(i);
        if (hasGroup(preset.groups, g))
            groups[developGroupKey(g)] = groupToJson(g, preset.values);
    }

    QJsonObject root;
    root["name"] = preset.name;
    root["version"] = 1;
    root["groups"] = groups;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

DevelopPreset parseDevelopPreset(const QByteArray& json, bool* ok) {
    DevelopPreset preset;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (ok)
            *ok = false;
        return preset;
    }

    const QJsonObject root = doc.object();
    preset.name = root.value("name").toString();

    const QJsonObject groups = root.value("groups").toObject();
    for (int i = 0; i < kDevelopGroupCount; ++i) {
        const char* key = developGroupKey(static_cast<DevelopGroup>(i));
        if (!groups.contains(key))
            continue;
        preset.groups.set(static_cast<size_t>(i));
        groupFromJson(static_cast<DevelopGroup>(i), groups.value(key).toObject(), preset.values);
    }

    if (ok)
        *ok = true;
    return preset;
}
