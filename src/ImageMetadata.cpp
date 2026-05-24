#include "ImageMetadata.h"
#include <libraw/libraw.h>
#include <QDateTime>
#include <cmath>

namespace {

void add(ImageMetadata& meta, const QString& label, const QString& value) {
    if (!value.isEmpty())
        meta.rows.append({label, value});
}

void add(ImageMetadata& meta, const QString& label, const char* value) {
    if (value && value[0])
        add(meta, label, QString::fromUtf8(value));
}

QString formatShutter(float seconds) {
    if (seconds <= 0.0f)
        return {};
    if (seconds < 1.0f) {
        const int denom = int(std::lround(1.0f / seconds));
        return denom > 0 ? QString("1/%1 s").arg(denom) : QString();
    }
    return QString("%1 s").arg(seconds, 0, 'f', seconds >= 10.0f ? 0 : 1);
}

QString formatAperture(float f) {
    if (f <= 0.0f)
        return {};
    return QString("f/%1").arg(f, 0, 'f', 1);
}

QString formatFocal(float mm) {
    if (mm <= 0.0f)
        return {};
    return QString("%1 mm").arg(mm, 0, 'f', mm >= 10.0f ? 0 : 1);
}

QString flipLabel(int flip) {
    switch (flip) {
    case 0:  return "None";
    case 3:  return "180°";
    case 5:  return "90° CW";
    case 6:  return "90° CCW";
    default: return QString::number(flip);
    }
}

QString exposureProgram(short program) {
    switch (program) {
    case 0:  return "Not defined";
    case 1:  return "Manual";
    case 2:  return "Normal program";
    case 3:  return "Aperture priority";
    case 4:  return "Shutter priority";
    case 5:  return "Creative program";
    case 6:  return "Action program";
    case 7:  return "Portrait mode";
    case 8:  return "Landscape mode";
    default: return {};
    }
}

QString meteringMode(short mode) {
    switch (mode) {
    case 0:  return "Unknown";
    case 1:  return "Average";
    case 2:  return "Center-weighted average";
    case 3:  return "Spot";
    case 4:  return "Multi-spot";
    case 5:  return "Pattern";
    case 6:  return "Partial";
    default: return {};
    }
}

QString gpsString(const libraw_gps_info_t& gps) {
    if (!gps.gpsparsed)
        return {};
    const auto dms = [](const float* d) {
        return QString("%1° %2' %3\"")
            .arg(int(d[0]))
            .arg(int(d[1]))
            .arg(d[2], 0, 'f', 1);
    };
    QString lat = dms(gps.latitude);
    QString lon = dms(gps.longitude);
    if (gps.latref == 'S') lat = lat + " S";
    else                   lat = lat + " N";
    if (gps.longref == 'W') lon = lon + " W";
    else                    lon = lon + " E";
    QString out = lat + ", " + lon;
    if (gps.altitude != 0.0f)
        out += QString("  (%1 m)").arg(gps.altitude, 0, 'f', 0);
    return out;
}

} // namespace

ImageMetadata extractMetadata(const LibRaw& raw) {
    ImageMetadata meta;
    const auto& id    = raw.imgdata.idata;
    const auto& sz    = raw.imgdata.sizes;
    const auto& other = raw.imgdata.other;
    const auto& lens  = raw.imgdata.lens;
    const auto& shoot = raw.imgdata.shootinginfo;

    add(meta, "Make",     id.make);
    add(meta, "Model",    id.model);
    add(meta, "Lens",     lens.Lens);
    add(meta, "Software", id.software);

    if (other.timestamp > 0) {
        const QDateTime dt = QDateTime::fromSecsSinceEpoch(other.timestamp);
        add(meta, "Date", dt.toString("yyyy-MM-dd HH:mm:ss"));
    }

    if (other.iso_speed > 0.0f)
        add(meta, "ISO", QString::number(int(std::lround(other.iso_speed))));
    add(meta, "Shutter",   formatShutter(other.shutter));
    add(meta, "Aperture",  formatAperture(other.aperture));
    add(meta, "Focal length", formatFocal(other.focal_len));
    if (lens.FocalLengthIn35mmFormat > 0)
        add(meta, "Focal length (35mm)",
            QString("%1 mm").arg(lens.FocalLengthIn35mmFormat));

    add(meta, "Exposure program", exposureProgram(shoot.ExposureProgram));
    add(meta, "Metering",         meteringMode(shoot.MeteringMode));
    add(meta, "Body serial",      shoot.BodySerial);

    add(meta, "Artist",      other.artist);
    add(meta, "Description", other.desc);
    add(meta, "GPS",         gpsString(other.parsed_gps));

    add(meta, "RAW size",
        QString("%1 × %2").arg(sz.raw_width).arg(sz.raw_height));
    add(meta, "Active area",
        QString("%1 × %2").arg(sz.width).arg(sz.height));
    add(meta, "Orientation", flipLabel(sz.flip));

    if (id.colors > 0)
        add(meta, "Channels", QString::number(id.colors));

    return meta;
}
