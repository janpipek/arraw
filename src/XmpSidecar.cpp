#include "XmpSidecar.h"
#include <QBuffer>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QRectF>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <type_traits>
#include <variant>

static constexpr char kNsCrs[] = "http://ns.adobe.com/camera-raw-settings/1.0/";
static constexpr char kNsRdf[] = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
static constexpr char kNsX[] = "adobe:ns:meta/";
static constexpr char kNsXmp[] = "http://ns.adobe.com/xap/1.0/";
// arraw-native develop data that has no Lightroom equivalent — local adjustments
// (docs/adr/0010). Versioned in the URI like Adobe's namespaces.
static constexpr char kNsArraw[] = "http://ns.arraw.app/1.0/";
static constexpr char kNsTiff[] = "http://ns.adobe.com/tiff/1.0/";

namespace {

enum class SidecarPathStatus { Missing, Selected, Ambiguous };

struct ResolvedSidecarPath {
    QString path;
    SidecarPathStatus status = SidecarPathStatus::Missing;
};

ResolvedSidecarPath resolveSidecarPath(const QString& rawPath) {
    const QFileInfo rawInfo(rawPath);
    const QString stemPath = rawInfo.dir().filePath(rawInfo.completeBaseName() + ".xmp");
    const QString extensionPath = rawPath + ".xmp";
    const bool stemExists = QFileInfo::exists(stemPath);
    const bool extensionExists = QFileInfo::exists(extensionPath);

    if (stemExists && extensionExists && stemPath != extensionPath)
        return {stemPath, SidecarPathStatus::Ambiguous};
    if (extensionExists)
        return {extensionPath, SidecarPathStatus::Selected};
    if (stemExists)
        return {stemPath, SidecarPathStatus::Selected};
    return {stemPath, SidecarPathStatus::Missing};
}

} // namespace

// crs: attribute names for the 8 HSL ranges, indexed like GlobalAdjustment::hslHue etc.
static constexpr const char* kHslHueNames[8]
    = {"HueAdjustmentRed",
       "HueAdjustmentOrange",
       "HueAdjustmentYellow",
       "HueAdjustmentGreen",
       "HueAdjustmentAqua",
       "HueAdjustmentBlue",
       "HueAdjustmentPurple",
       "HueAdjustmentMagenta"};
static constexpr const char* kHslSatNames[8]
    = {"SaturationAdjustmentRed",
       "SaturationAdjustmentOrange",
       "SaturationAdjustmentYellow",
       "SaturationAdjustmentGreen",
       "SaturationAdjustmentAqua",
       "SaturationAdjustmentBlue",
       "SaturationAdjustmentPurple",
       "SaturationAdjustmentMagenta"};
static constexpr const char* kHslLumNames[8]
    = {"LuminanceAdjustmentRed",
       "LuminanceAdjustmentOrange",
       "LuminanceAdjustmentYellow",
       "LuminanceAdjustmentGreen",
       "LuminanceAdjustmentAqua",
       "LuminanceAdjustmentBlue",
       "LuminanceAdjustmentPurple",
       "LuminanceAdjustmentMagenta"};

QString XmpSidecar::pathFor(const QString& rawPath) {
    return resolveSidecarPath(rawPath).path;
}

GlobalAdjustment XmpSidecar::resolveAdjustments(
    const QString& rawPath, const QRectF& defaultCrop, orient::Orientation seededOrientation) {
    return resolveAdjustmentsWithStatus(rawPath, defaultCrop, seededOrientation).adjustments;
}

SidecarLoadResult XmpSidecar::resolveForImage(
    const QString& rawPath, const QRectF& defaultCrop, orient::Orientation seededOrientation) {
    SidecarLoadResult loaded = loadWithStatus(rawPath);
    if (loaded.status != SidecarLoadStatus::Loaded) {
        loaded.data = {};
        loaded.data.adjustments.cropRect = defaultCrop;
    }
    // Precedence (docs/adr/0028): a stored tiff:Orientation wins; otherwise seed
    // from the file's EXIF — also the migration path for pre-orientation sidecars.
    if (!loaded.data.orientationStored)
        loaded.data.adjustments.orientation = seededOrientation;
    return loaded;
}

SidecarAdjustmentResult XmpSidecar::resolveAdjustmentsWithStatus(
    const QString& rawPath, const QRectF& defaultCrop, orient::Orientation seededOrientation) {
    const SidecarLoadResult loaded = resolveForImage(rawPath, defaultCrop, seededOrientation);
    return {loaded.data.adjustments, loaded.status};
}

// Parse "x, y" pairs from an rdf:Seq element into control points (0..255 scale → 0..1).
static std::vector<QPointF> parseCurveSeq(QXmlStreamReader& xml) {
    std::vector<QPointF> pts;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.qualifiedName() == "rdf:Seq")
            break;
        if (xml.isStartElement() && xml.qualifiedName() == "rdf:li") {
            const QString text = xml.readElementText().trimmed();
            const int comma = text.indexOf(',');
            if (comma > 0) {
                bool ok1, ok2;
                const float x = text.left(comma).trimmed().toFloat(&ok1);
                const float y = text.mid(comma + 1).trimmed().toFloat(&ok2);
                if (ok1 && ok2)
                    pts.push_back({x / 255.0, y / 255.0});
            }
        }
    }
    if (pts.size() < 2)
        pts = {{0.0, 0.0}, {1.0, 1.0}};
    return pts;
}

// Parse one rdf:li (parseType="Resource") of the arraw:LocalAdjustments Seq.
// The reader is positioned on the rdf:li start element.
static LocalAdjustment parseLocalAdjustmentLi(QXmlStreamReader& xml) {
    LocalAdjustment la;
    QString maskType = "Linear";
    QPointF p0, p1;
    QPointF center{0.5, 0.5};
    double radiusX = 0.25, radiusY = 0.25, angle = 0.0, feather = 0.5;
    bool invert = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.qualifiedName() == "rdf:li")
            break;
        if (!xml.isStartElement())
            continue;
        const QString name = xml.qualifiedName().toString();
        const QString text = xml.readElementText();
        const float v = text.toFloat();
        if (name == "arraw:MaskType")
            maskType = text.trimmed();
        else if (name == "arraw:P0x")
            p0.setX(v);
        else if (name == "arraw:P0y")
            p0.setY(v);
        else if (name == "arraw:P1x")
            p1.setX(v);
        else if (name == "arraw:P1y")
            p1.setY(v);
        else if (name == "arraw:CenterX")
            center.setX(v);
        else if (name == "arraw:CenterY")
            center.setY(v);
        else if (name == "arraw:RadiusX")
            radiusX = v;
        else if (name == "arraw:RadiusY")
            radiusY = v;
        else if (name == "arraw:Angle")
            angle = v;
        else if (name == "arraw:Feather")
            feather = v;
        else if (name == "arraw:Invert")
            invert = (v != 0.0f);
        else if (name == "arraw:Exposure")
            la.exposure = v;
        else if (name == "arraw:Contrast")
            la.contrast = v;
        else if (name == "arraw:Highlights")
            la.highlights = v;
        else if (name == "arraw:Shadows")
            la.shadows = v;
        else if (name == "arraw:Whites")
            la.whites = v;
        else if (name == "arraw:Blacks")
            la.blacks = v;
        else if (name == "arraw:Temperature")
            la.temperature = v;
        else if (name == "arraw:Tint")
            la.tint = v;
        else if (name == "arraw:Saturation")
            la.saturation = v;
        else if (name == "arraw:Vibrance")
            la.vibrance = v;
    }
    if (maskType == "Radial")
        la.mask = RadialMask{center, radiusX, radiusY, angle, feather, invert};
    else
        la.mask = LinearMask{p0, p1};
    return la;
}

// Parse one rdf:li (parseType="Resource") of the arraw:Spots Seq.
static Spot parseSpotLi(QXmlStreamReader& xml) {
    Spot s;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.qualifiedName() == "rdf:li")
            break;
        if (!xml.isStartElement())
            continue;
        const auto name = xml.qualifiedName().toString();
        const QString text = xml.readElementText();
        bool ok = false;
        const double v = text.toDouble(&ok);
        if (!ok)
            continue;
        if (name == "arraw:DestX")
            s.destination.setX(v);
        else if (name == "arraw:DestY")
            s.destination.setY(v);
        else if (name == "arraw:SourceX")
            s.source.setX(v);
        else if (name == "arraw:SourceY")
            s.source.setY(v);
        else if (name == "arraw:Radius")
            s.radius = v;
        else if (name == "arraw:Feather")
            s.feather = v;
    }
    return s;
}

// Parse the arraw:Spots Seq into a list. Positioned on the Spots start element.
static std::vector<Spot> parseSpots(QXmlStreamReader& xml) {
    std::vector<Spot> out;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.qualifiedName() == "arraw:Spots")
            break;
        if (xml.isStartElement() && xml.qualifiedName() == "rdf:li")
            out.push_back(parseSpotLi(xml));
    }
    return out;
}

// Parse the arraw:LocalAdjustments Seq into a list. Positioned on the
// LocalAdjustments start element. Honours the 16-mask cap (docs/adr/0010).
static std::vector<LocalAdjustment> parseLocalAdjustments(QXmlStreamReader& xml) {
    std::vector<LocalAdjustment> out;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.qualifiedName() == "arraw:LocalAdjustments")
            break;
        if (xml.isStartElement() && xml.qualifiedName() == "rdf:li" && out.size() < 16)
            out.push_back(parseLocalAdjustmentLi(xml));
    }
    return out;
}

SidecarData XmpSidecar::load(const QString& rawPath) {
    return loadWithStatus(rawPath).data;
}

SidecarLoadResult XmpSidecar::loadWithStatus(const QString& rawPath) {
    const ResolvedSidecarPath resolved = resolveSidecarPath(rawPath);
    if (resolved.status == SidecarPathStatus::Ambiguous)
        return {{}, SidecarLoadStatus::ParseError};
    if (resolved.status == SidecarPathStatus::Missing)
        return {{}, SidecarLoadStatus::Missing};
    QFile f(resolved.path);
    if (!f.open(QIODevice::ReadOnly))
        return {{}, SidecarLoadStatus::ParseError};

    SidecarData data;
    GlobalAdjustment& p = data.adjustments;
    QXmlStreamReader xml(&f);

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement() && xml.qualifiedName() == "rdf:Description") {
            auto attr = [&](const char* name, float fallback) -> float {
                auto val = xml.attributes().value(kNsCrs, name);
                if (val.isEmpty())
                    return fallback;
                bool ok = false;
                float v = val.toFloat(&ok);
                return ok ? v : fallback;
            };

            // User metadata (xmp: namespace): rating + colour label.
            if (auto r = xml.attributes().value(kNsXmp, "Rating"); !r.isEmpty()) {
                bool ok = false;
                int v = r.toInt(&ok);
                if (ok)
                    data.metadata.rating = v;
            }
            data.metadata.label = colourLabelFromString(
                xml.attributes().value(kNsXmp, "Label").toString());
            p.exposure = attr("Exposure2012", 0.0f);
            p.contrast = attr("Contrast2012", 0.0f);
            p.highlights = attr("Highlights2012", 0.0f);
            p.shadows = attr("Shadows2012", 0.0f);
            p.whites = attr("Whites2012", 0.0f);
            p.blacks = attr("Blacks2012", 0.0f);
            p.temperature = attr("Temperature", 5500.0f);
            p.tint = attr("Tint", 0.0f);
            p.saturation = attr("Saturation", 0.0f);
            p.vibrance = attr("Vibrance", 0.0f);
            p.sharpening = attr("Sharpness", 0.0f);
            p.rotation = attr("CropAngle", 0.0f);
            p.postCropVignetteAmount = attr("PostCropVignetteAmount", 0.0f);
            p.postCropVignetteMidpoint = attr("PostCropVignetteMidpoint", 50.0f);
            p.postCropVignetteFeather = attr("PostCropVignetteFeather", 50.0f);
            p.grainAmount = attr("GrainAmount", 0.0f);
            p.grainSize = attr("GrainSize", 50.0f);
            p.grainRoughness = attr("GrainFrequency", 50.0f);
            if (const auto seed = xml.attributes().value(kNsArraw, "GrainSeed"); !seed.isEmpty()) {
                bool ok = false;
                const auto value = seed.toUInt(&ok);
                if (ok)
                    p.grainSeed = value;
            }
            // Lens Corrections enable toggles (docs/adr/0027). arraw-owned; the profile
            // identity/coefficients are re-derived from the file on load, not stored.
            const auto boolAttr = [&](const char* name) {
                return xml.attributes().value(kNsArraw, name).toString().compare(
                           "true", Qt::CaseInsensitive)
                    == 0;
            };
            p.lensCorrectDistortion = boolAttr("LensCorrectDistortion");
            p.lensCorrectVignetting = boolAttr("LensCorrectVignetting");
            p.lensCorrectCA = boolAttr("LensCorrectCA");
            // Coarse orientation: the standard EXIF/tiff enum. Absent means the
            // decode layer seeds it from EXIF instead (docs/adr/0028), so leave
            // the default identity here when the attribute is missing.
            if (auto o = xml.attributes().value(kNsTiff, "Orientation"); !o.isEmpty()) {
                bool ok = false;
                const int exif = o.toInt(&ok);
                if (ok) {
                    p.orientation = orient::fromExif(exif);
                    data.orientationStored = true;
                }
            }
            // crs stores the crop as normalised edges (left/top/right/bottom),
            // QRectF wants x/y/width/height.
            p.cropRect = QRectF(
                attr("CropLeft", 0.0f),
                attr("CropTop", 0.0f),
                attr("CropRight", 1.0f) - attr("CropLeft", 0.0f),
                attr("CropBottom", 1.0f) - attr("CropTop", 0.0f));
            p.cropConstrained = xml.attributes()
                                    .value(kNsCrs, "CropConstrainAspectRatio")
                                    .toString()
                                    .compare("true", Qt::CaseInsensitive)
                                == 0;

            // HSL attributes
            for (int i = 0; i < 8; ++i) {
                p.hslHue[i] = attr(kHslHueNames[i], 0.0f);
                p.hslSat[i] = attr(kHslSatNames[i], 0.0f);
                p.hslLum[i] = attr(kHslLumNames[i], 0.0f);
            }
        }

        // Tone curve child elements (inside rdf:Description)
        if (xml.isStartElement()) {
            const auto name = xml.qualifiedName().toString();
            CurvePoints* target = nullptr;
            if (name == "crs:ToneCurvePV2012")
                target = &p.curveLuma;
            else if (name == "crs:ToneCurvePV2012Red")
                target = &p.curveR;
            else if (name == "crs:ToneCurvePV2012Green")
                target = &p.curveG;
            else if (name == "crs:ToneCurvePV2012Blue")
                target = &p.curveB;
            if (target) {
                // Enter rdf:Seq — skip the whitespace tokens that formatted
                // XML (including our own auto-formatted output) puts between
                // the curve element and its child.
                do {
                    xml.readNext();
                } while (xml.isWhitespace());
                if (xml.isStartElement() && xml.qualifiedName() == "rdf:Seq")
                    target->points = parseCurveSeq(xml);
            }

            // arraw-native local adjustments (docs/adr/0010).
            if (name == "arraw:LocalAdjustments")
                p.localAdjustments = parseLocalAdjustments(xml);
            // arraw-native spots (docs/adr/0017).
            if (name == "arraw:Spots")
                p.spots = parseSpots(xml);
        }
    }
    if (xml.hasError())
        return {{}, SidecarLoadStatus::ParseError};
    return {data, SidecarLoadStatus::Loaded};
}

static void writeCurve(QXmlStreamWriter& xml, const char* elemName, const std::vector<QPointF>& pts) {
    // Skip if identity (only the two default endpoints, unmodified)
    if (isIdentityCurve(pts))
        return;

    xml.writeStartElement(kNsCrs, elemName);
    xml.writeStartElement(kNsRdf, "Seq");
    for (const auto& pt : pts) {
        xml.writeStartElement(kNsRdf, "li");
        xml.writeCharacters(
            QString::number(qRound(pt.x() * 255.0)) + ", "
            + QString::number(qRound(pt.y() * 255.0)));
        xml.writeEndElement();
    }
    xml.writeEndElement(); // Seq
    xml.writeEndElement(); // curve element
}

// Writes arraw-native spots as a Seq of struct resources (docs/adr/0017).
// Each li carries destination + source pixel coordinates, radius, and feather.
static void writeSpots(QXmlStreamWriter& xml, const std::vector<Spot>& spots) {
    if (spots.empty())
        return;
    auto num = [](double v) { return QString::number(v, 'f', 4); };
    xml.writeStartElement(kNsArraw, "Spots");
    xml.writeStartElement(kNsRdf, "Seq");
    for (const auto& s : spots) {
        xml.writeStartElement(kNsRdf, "li");
        xml.writeAttribute(kNsRdf, "parseType", "Resource");
        xml.writeTextElement(kNsArraw, "DestX", num(s.destination.x()));
        xml.writeTextElement(kNsArraw, "DestY", num(s.destination.y()));
        xml.writeTextElement(kNsArraw, "SourceX", num(s.source.x()));
        xml.writeTextElement(kNsArraw, "SourceY", num(s.source.y()));
        xml.writeTextElement(kNsArraw, "Radius", num(s.radius));
        xml.writeTextElement(kNsArraw, "Feather", num(s.feather));
        xml.writeEndElement(); // rdf:li
    }
    xml.writeEndElement(); // rdf:Seq
    xml.writeEndElement(); // arraw:Spots
}

// Writes the arraw-native local adjustments as a Seq of struct resources
// (docs/adr/0010). Each li carries the mask type + geometry and the shared
// delta subset; Temperature here is a relative -100..100 shift, not Kelvin.
static void writeLocalAdjustments(QXmlStreamWriter& xml, const std::vector<LocalAdjustment>& las) {
    if (las.empty())
        return;
    auto num = [](float v) { return QString::number(double(v), 'f', 4); };

    xml.writeStartElement(kNsArraw, "LocalAdjustments");
    xml.writeStartElement(kNsRdf, "Seq");
    for (const auto& la : las) {
        xml.writeStartElement(kNsRdf, "li");
        xml.writeAttribute(kNsRdf, "parseType", "Resource");

        std::visit(
            [&](const auto& mask) {
                using T = std::decay_t<decltype(mask)>;
                if constexpr (std::is_same_v<T, LinearMask>) {
                    xml.writeTextElement(kNsArraw, "MaskType", "Linear");
                    xml.writeTextElement(kNsArraw, "P0x", num(mask.p0.x()));
                    xml.writeTextElement(kNsArraw, "P0y", num(mask.p0.y()));
                    xml.writeTextElement(kNsArraw, "P1x", num(mask.p1.x()));
                    xml.writeTextElement(kNsArraw, "P1y", num(mask.p1.y()));
                } else if constexpr (std::is_same_v<T, RadialMask>) {
                    xml.writeTextElement(kNsArraw, "MaskType", "Radial");
                    xml.writeTextElement(kNsArraw, "CenterX", num(mask.center.x()));
                    xml.writeTextElement(kNsArraw, "CenterY", num(mask.center.y()));
                    xml.writeTextElement(kNsArraw, "RadiusX", num(mask.radiusX));
                    xml.writeTextElement(kNsArraw, "RadiusY", num(mask.radiusY));
                    xml.writeTextElement(kNsArraw, "Angle", num(mask.angle));
                    xml.writeTextElement(kNsArraw, "Feather", num(mask.feather));
                    xml.writeTextElement(kNsArraw, "Invert", mask.invert ? "1" : "0");
                }
            },
            la.mask);

        xml.writeTextElement(kNsArraw, "Exposure", num(la.exposure));
        xml.writeTextElement(kNsArraw, "Contrast", num(la.contrast));
        xml.writeTextElement(kNsArraw, "Highlights", num(la.highlights));
        xml.writeTextElement(kNsArraw, "Shadows", num(la.shadows));
        xml.writeTextElement(kNsArraw, "Whites", num(la.whites));
        xml.writeTextElement(kNsArraw, "Blacks", num(la.blacks));
        xml.writeTextElement(kNsArraw, "Temperature", num(la.temperature));
        xml.writeTextElement(kNsArraw, "Tint", num(la.tint));
        xml.writeTextElement(kNsArraw, "Saturation", num(la.saturation));
        xml.writeTextElement(kNsArraw, "Vibrance", num(la.vibrance));

        xml.writeEndElement(); // rdf:li
    }
    xml.writeEndElement(); // rdf:Seq
    xml.writeEndElement(); // arraw:LocalAdjustments
}

// Writes the whole sidecar (both develop settings and user metadata) from one
// SidecarData. The namespace-scoped public saves below read-modify-write through
// this, so each preserves the half it doesn't touch (docs/adr/0007).
static QByteArray ownedPacket(const SidecarData& data) {
    const GlobalAdjustment& p = data.adjustments;

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);

    QXmlStreamWriter xml(&buffer);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeProcessingInstruction("xpacket", R"(begin="" id="W5M0MpCehiHzreSzNTczkc9d")");

    xml.writeNamespace(kNsX, "x");
    xml.writeStartElement(kNsX, "xmpmeta");

    xml.writeNamespace(kNsRdf, "rdf");
    xml.writeStartElement(kNsRdf, "RDF");

    xml.writeNamespace(kNsRdf, "rdf");
    xml.writeNamespace(kNsCrs, "crs");
    xml.writeNamespace(kNsXmp, "xmp");
    xml.writeNamespace(kNsArraw, "arraw");
    xml.writeNamespace(kNsTiff, "tiff");
    xml.writeStartElement(kNsRdf, "Description");
    xml.writeAttribute(kNsRdf, "about", "");

    // User metadata (xmp:). Absent attribute = unrated / unlabelled, so the
    // defaults are omitted to keep an unmarked sidecar clean.
    if (data.metadata.rating != 0)
        xml.writeAttribute(kNsXmp, "Rating", QString::number(data.metadata.rating));
    if (data.metadata.label != ColourLabel::None)
        xml.writeAttribute(kNsXmp, "Label", colourLabelToString(data.metadata.label));

    auto write = [&](const char* name, float v) {
        xml.writeAttribute(kNsCrs, name, QString::number(double(v), 'f', 4));
    };
    write("Exposure2012", p.exposure);
    write("Contrast2012", p.contrast);
    write("Highlights2012", p.highlights);
    write("Shadows2012", p.shadows);
    write("Whites2012", p.whites);
    write("Blacks2012", p.blacks);
    write("Temperature", p.temperature);
    write("Tint", p.tint);
    write("Saturation", p.saturation);
    write("Vibrance", p.vibrance);
    write("Sharpness", p.sharpening);
    write("CropAngle", p.rotation); // Adobe's real straighten field (docs/adr/0028)
    write("PostCropVignetteAmount", p.postCropVignetteAmount);
    write("PostCropVignetteMidpoint", p.postCropVignetteMidpoint);
    write("PostCropVignetteFeather", p.postCropVignetteFeather);
    write("GrainAmount", p.grainAmount);
    write("GrainSize", p.grainSize);
    write("GrainFrequency", p.grainRoughness);
    if (p.grainSeed != 0)
        xml.writeAttribute(kNsArraw, "GrainSeed", QString::number(p.grainSeed));
    // Lens Corrections toggles (docs/adr/0027); written only when on (default off).
    if (p.lensCorrectDistortion)
        xml.writeAttribute(kNsArraw, "LensCorrectDistortion", "True");
    if (p.lensCorrectVignetting)
        xml.writeAttribute(kNsArraw, "LensCorrectVignetting", "True");
    if (p.lensCorrectCA)
        xml.writeAttribute(kNsArraw, "LensCorrectCA", "True");
    write("CropLeft", float(p.cropRect.left()));
    write("CropTop", float(p.cropRect.top()));
    write("CropRight", float(p.cropRect.right()));
    write("CropBottom", float(p.cropRect.bottom()));
    xml.writeAttribute(kNsCrs, "CropConstrainAspectRatio", p.cropConstrained ? "True" : "False");
    // Coarse orientation as the standard EXIF/tiff enum 1..8 (docs/adr/0028).
    xml.writeAttribute(kNsTiff, "Orientation", QString::number(orient::toExif(p.orientation)));

    // HSL
    for (int i = 0; i < 8; ++i) {
        write(kHslHueNames[i], p.hslHue[i]);
        write(kHslSatNames[i], p.hslSat[i]);
        write(kHslLumNames[i], p.hslLum[i]);
    }

    // Tone curves (child elements, written after attributes)
    writeCurve(xml, "ToneCurvePV2012", p.curveLuma.points);
    writeCurve(xml, "ToneCurvePV2012Red", p.curveR.points);
    writeCurve(xml, "ToneCurvePV2012Green", p.curveG.points);
    writeCurve(xml, "ToneCurvePV2012Blue", p.curveB.points);

    // arraw-native local adjustments (docs/adr/0010).
    writeLocalAdjustments(xml, p.localAdjustments);
    // arraw-native spots (docs/adr/0017).
    writeSpots(xml, p.spots);

    xml.writeEndElement(); // rdf:Description
    xml.writeEndElement(); // rdf:RDF
    xml.writeEndElement(); // x:xmpmeta
    xml.writeProcessingInstruction("xpacket", "end=\"w\"");
    xml.writeEndDocument();

    return xml.hasError() ? QByteArray{} : bytes;
}

enum class SaveScope { Adjustments, Metadata };

static QDomElement firstDescription(QDomDocument& document) {
    const QDomNodeList descriptions = document.elementsByTagNameNS(kNsRdf, "Description");
    return descriptions.isEmpty() ? QDomElement{} : descriptions.at(0).toElement();
}

static bool parseDocument(const QByteArray& bytes, QDomDocument& document) {
    return bool(document.setContent(bytes, QDomDocument::ParseOption::UseNamespaceProcessing));
}

static void removeNamespaceContent(QDomNode node, const QString& namespaceUri) {
    if (node.isElement()) {
        QDomElement element = node.toElement();
        QDomNamedNodeMap attributes = element.attributes();
        for (int i = attributes.size() - 1; i >= 0; --i) {
            const QDomAttr attribute = attributes.item(i).toAttr();
            if (attribute.namespaceURI() == namespaceUri)
                element.removeAttributeNode(attribute);
        }
    }

    for (QDomNode child = node.firstChild(); !child.isNull();) {
        const QDomNode next = child.nextSibling();
        if (child.isElement() && child.namespaceURI() == namespaceUri)
            node.removeChild(child);
        else
            removeNamespaceContent(child, namespaceUri);
        child = next;
    }
}

static void removeOwnedAttributes(
    QDomDocument& document, const QString& namespaceUri, const QStringList& names) {
    const QDomNodeList descriptions = document.elementsByTagNameNS(kNsRdf, "Description");
    for (int i = 0; i < descriptions.size(); ++i) {
        QDomElement description = descriptions.at(i).toElement();
        for (const QString& name : names)
            description.removeAttributeNS(namespaceUri, name);
    }
}

static void removeOwnedElements(
    QDomDocument& document, const QString& namespaceUri, const QStringList& names) {
    const QDomNodeList descriptions = document.elementsByTagNameNS(kNsRdf, "Description");
    for (int i = 0; i < descriptions.size(); ++i) {
        QDomElement description = descriptions.at(i).toElement();
        for (QDomNode child = description.firstChild(); !child.isNull();) {
            const QDomNode next = child.nextSibling();
            if (child.namespaceURI() == namespaceUri && names.contains(child.localName()))
                description.removeChild(child);
            child = next;
        }
    }
}

static void importAttributes(
    QDomDocument& destination,
    QDomElement target,
    const QDomElement& source,
    const QString& namespaceUri,
    const QStringList& names = {}) {
    const QDomNamedNodeMap attributes = source.attributes();
    for (int i = 0; i < attributes.size(); ++i) {
        const QDomAttr attribute = attributes.item(i).toAttr();
        if (attribute.namespaceURI() == namespaceUri
            && (names.isEmpty() || names.contains(attribute.localName()))) {
            target.setAttributeNodeNS(destination.importNode(attribute, true).toAttr());
        }
    }
}

static void importElements(
    QDomDocument& destination,
    QDomElement target,
    const QDomElement& source,
    const QString& namespaceUri,
    const QStringList& names = {}) {
    for (QDomNode child = source.firstChild(); !child.isNull(); child = child.nextSibling()) {
        if (child.namespaceURI() == namespaceUri
            && (names.isEmpty() || names.contains(child.localName()))) {
            target.appendChild(destination.importNode(child, true));
        }
    }
}

static bool mergeOwnedPacket(QDomDocument& document, const QByteArray& packet, SaveScope scope) {
    QDomDocument owned;
    if (!parseDocument(packet, owned))
        return false;

    QDomElement target = firstDescription(document);
    const QDomElement source = firstDescription(owned);
    if (target.isNull() || source.isNull())
        return false;

    if (scope == SaveScope::Metadata) {
        const QStringList names = {"Rating", "Label"};
        removeOwnedAttributes(document, kNsXmp, names);
        importAttributes(document, target, source, kNsXmp, names);
        return true;
    }

    QStringList crsAttributes;
    const QDomNamedNodeMap sourceAttributes = source.attributes();
    for (int i = 0; i < sourceAttributes.size(); ++i) {
        const QDomAttr attribute = sourceAttributes.item(i).toAttr();
        if (attribute.namespaceURI() == kNsCrs)
            crsAttributes.append(attribute.localName());
    }
    const QStringList crsElements = {
        "ToneCurvePV2012", "ToneCurvePV2012Red", "ToneCurvePV2012Green", "ToneCurvePV2012Blue"};
    removeOwnedAttributes(document, kNsCrs, crsAttributes);
    removeOwnedElements(document, kNsCrs, crsElements);
    removeNamespaceContent(document, kNsArraw);
    importAttributes(document, target, source, kNsCrs);
    importElements(document, target, source, kNsCrs, crsElements);
    importAttributes(document, target, source, kNsArraw);
    importElements(document, target, source, kNsArraw);
    return true;
}

static bool writeFile(const QString& rawPath, const SidecarData& data, SaveScope scope) {
    const ResolvedSidecarPath resolved = resolveSidecarPath(rawPath);
    if (resolved.status == SidecarPathStatus::Ambiguous)
        return false;
    const QString& path = resolved.path;
    const QByteArray packet = ownedPacket(data);
    if (packet.isEmpty())
        return false;

    QByteArray output = packet;
    if (QFile::exists(path)) {
        QFile existing(path);
        if (!existing.open(QIODevice::ReadOnly))
            return false;
        QDomDocument document;
        if (!parseDocument(existing.readAll(), document) || !mergeOwnedPacket(document, packet, scope))
            return false;
        output = document.toByteArray(2);
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(output) != output.size())
        return false;
    return file.commit();
}

bool XmpSidecar::saveAdjustments(const QString& rawPath, const GlobalAdjustment& params) {
    SidecarData data = load(rawPath); // preserve any existing xmp: marks
    data.adjustments = params;
    return writeFile(rawPath, data, SaveScope::Adjustments);
}

bool XmpSidecar::saveMetadata(const QString& rawPath, const UserMetadata& metadata) {
    SidecarData data = load(rawPath); // preserve any existing crs: edits
    data.metadata = metadata;
    return writeFile(rawPath, data, SaveScope::Metadata);
}
