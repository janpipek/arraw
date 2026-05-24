#include "XmpSidecar.h"
#include <QFile>
#include <QRectF>
#include <QFileInfo>
#include <QDir>
#include <QXmlStreamWriter>
#include <QXmlStreamReader>

static constexpr char kNsCrs[]  = "http://ns.adobe.com/camera-raw-settings/1.0/";
static constexpr char kNsRdf[]  = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
static constexpr char kNsX[]    = "adobe:ns:meta/";

QString XmpSidecar::pathFor(const QString& rawPath) {
    QFileInfo fi(rawPath);
    return fi.dir().filePath(fi.completeBaseName() + ".xmp");
}

AdjustmentParams XmpSidecar::load(const QString& rawPath) {
    QFile f(pathFor(rawPath));
    if (!f.open(QIODevice::ReadOnly)) return {};

    AdjustmentParams p;
    QXmlStreamReader xml(&f);

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.qualifiedName() == "rdf:Description") {
            auto attr = [&](const char* name, float fallback) -> float {
                auto val = xml.attributes().value(kNsCrs, name);
                if (val.isEmpty()) return fallback;
                bool ok = false;
                float v = val.toFloat(&ok);
                return ok ? v : fallback;
            };
            p.exposure    = attr("Exposure2012",   0.0f);
            p.contrast    = attr("Contrast2012",   0.0f);
            p.highlights  = attr("Highlights2012", 0.0f);
            p.shadows     = attr("Shadows2012",    0.0f);
            p.whites      = attr("Whites2012",     0.0f);
            p.blacks      = attr("Blacks2012",     0.0f);
            p.temperature = attr("Temperature",    5500.0f);
            p.tint        = attr("Tint",           0.0f);
            p.saturation  = attr("Saturation",     0.0f);
            p.vibrance    = attr("Vibrance",       0.0f);
            p.sharpening  = attr("Sharpness",      0.0f);
            p.rotation    = attr("StraightenAngle",0.0f);
            p.cropRect    = QRectF(
                attr("CropLeft",   0.0f), attr("CropTop",    0.0f),
                attr("CropRight",  1.0f) - attr("CropLeft", 0.0f),
                attr("CropBottom", 1.0f) - attr("CropTop",  0.0f));
            break;
        }
    }
    return p;
}

bool XmpSidecar::save(const QString& rawPath, const AdjustmentParams& p) {
    QFile f(pathFor(rawPath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;

    QXmlStreamWriter xml(&f);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeProcessingInstruction("xpacket", R"(begin="" id="W5M0MpCehiHzreSzNTczkc9d")");

    xml.writeNamespace(kNsX,   "x");
    xml.writeStartElement(kNsX, "xmpmeta");

    xml.writeNamespace(kNsRdf, "rdf");
    xml.writeStartElement(kNsRdf, "RDF");

    xml.writeNamespace(kNsRdf, "rdf");
    xml.writeNamespace(kNsCrs, "crs");
    xml.writeStartElement(kNsRdf, "Description");
    xml.writeAttribute(kNsRdf, "about", "");

    auto write = [&](const char* name, float v) {
        xml.writeAttribute(kNsCrs, name, QString::number(double(v), 'f', 4));
    };
    write("Exposure2012",   p.exposure);
    write("Contrast2012",   p.contrast);
    write("Highlights2012", p.highlights);
    write("Shadows2012",    p.shadows);
    write("Whites2012",     p.whites);
    write("Blacks2012",     p.blacks);
    write("Temperature",    p.temperature);
    write("Tint",           p.tint);
    write("Saturation",     p.saturation);
    write("Vibrance",       p.vibrance);
    write("Sharpness",       p.sharpening);
    write("StraightenAngle", p.rotation);
    write("CropLeft",        float(p.cropRect.left()));
    write("CropTop",         float(p.cropRect.top()));
    write("CropRight",       float(p.cropRect.right()));
    write("CropBottom",      float(p.cropRect.bottom()));

    xml.writeEndElement(); // rdf:Description
    xml.writeEndElement(); // rdf:RDF
    xml.writeEndElement(); // x:xmpmeta
    xml.writeProcessingInstruction("xpacket", "end=\"w\"");
    xml.writeEndDocument();

    return f.error() == QFileDevice::NoError;
}
