#include "DevelopPreset.h"

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
    case DevelopGroup::Detail:
        o["sharpening"] = v.sharpening;
        break;
    case DevelopGroup::Geometry:
        o["rotation"] = v.rotation;
        o["crop"]
            = QJsonArray{v.cropRect.x(), v.cropRect.y(), v.cropRect.width(), v.cropRect.height()};
        o["constrained"] = v.cropConstrained;
        break;
    case DevelopGroup::Effects:
        o["vignetteAmount"] = v.vignetteAmount;
        o["vignetteMidpoint"] = v.vignetteMidpoint;
        o["vignetteFeather"] = v.vignetteFeather;
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
    case DevelopGroup::Detail:
        v.sharpening = f("sharpening", v.sharpening);
        break;
    case DevelopGroup::Geometry: {
        v.rotation = f("rotation", v.rotation);
        const QJsonArray c = o["crop"].toArray();
        if (c.size() == 4)
            v.cropRect = QRectF(
                c.at(0).toDouble(), c.at(1).toDouble(), c.at(2).toDouble(), c.at(3).toDouble());
        v.cropConstrained = o.value("constrained").toBool(v.cropConstrained);
        break;
    }
    case DevelopGroup::Effects:
        v.vignetteAmount = f("vignetteAmount", v.vignetteAmount);
        v.vignetteMidpoint = f("vignetteMidpoint", v.vignetteMidpoint);
        v.vignetteFeather = f("vignetteFeather", v.vignetteFeather);
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
