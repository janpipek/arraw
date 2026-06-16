#include "XmpSidecar.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRectF>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

static constexpr char kNsCrs[] = "http://ns.adobe.com/camera-raw-settings/1.0/";
static constexpr char kNsRdf[] = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
static constexpr char kNsX[] = "adobe:ns:meta/";
static constexpr char kNsXmp[] = "http://ns.adobe.com/xap/1.0/";

// crs: attribute names for the 8 HSL ranges, indexed like AdjustmentParams::hslHue etc.
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
    QFileInfo fi(rawPath);
    return fi.dir().filePath(fi.completeBaseName() + ".xmp");
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

SidecarData XmpSidecar::load(const QString& rawPath) {
    QFile f(pathFor(rawPath));
    if (!f.open(QIODevice::ReadOnly))
        return {};

    SidecarData data;
    AdjustmentParams& p = data.adjustments;
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
            p.rotation = attr("StraightenAngle", 0.0f);
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
        }
    }
    return data;
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

// Writes the whole sidecar (both develop settings and user metadata) from one
// SidecarData. The namespace-scoped public saves below read-modify-write through
// this, so each preserves the half it doesn't touch (docs/adr/0007).
static bool writeFile(const QString& rawPath, const SidecarData& data) {
    const AdjustmentParams& p = data.adjustments;

    QFile f(XmpSidecar::pathFor(rawPath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    QXmlStreamWriter xml(&f);
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
    write("StraightenAngle", p.rotation);
    write("CropLeft", float(p.cropRect.left()));
    write("CropTop", float(p.cropRect.top()));
    write("CropRight", float(p.cropRect.right()));
    write("CropBottom", float(p.cropRect.bottom()));
    xml.writeAttribute(kNsCrs, "CropConstrainAspectRatio", p.cropConstrained ? "True" : "False");

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

    xml.writeEndElement(); // rdf:Description
    xml.writeEndElement(); // rdf:RDF
    xml.writeEndElement(); // x:xmpmeta
    xml.writeProcessingInstruction("xpacket", "end=\"w\"");
    xml.writeEndDocument();

    return f.error() == QFileDevice::NoError;
}

bool XmpSidecar::saveAdjustments(const QString& rawPath, const AdjustmentParams& params) {
    SidecarData data = load(rawPath); // preserve any existing xmp: marks
    data.adjustments = params;
    return writeFile(rawPath, data);
}

bool XmpSidecar::saveMetadata(const QString& rawPath, const UserMetadata& metadata) {
    SidecarData data = load(rawPath); // preserve any existing crs: edits
    data.metadata = metadata;
    return writeFile(rawPath, data);
}
