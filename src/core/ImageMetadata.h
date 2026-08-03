#pragma once
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QVector>

class LibRaw;

// Camera EXIF in native units and native types — the machine-readable half,
// exposed under stable keys by `arraw info --json` (docs/adr/0053) so a script
// can filter on `iso > 1600` without parsing "f/2.8" back into 2.8.
// ImageMetadata's display rows are formatted *from* these, so each underlying
// LibRaw field is read in exactly one place.
struct ExifData {
    QString make;
    QString model;
    QString lens;
    int iso = 0;                      // 0 when absent
    float apertureFNumber = 0.0f;     // f-number, e.g. 2.8
    float focalLengthMm = 0.0f;       // millimetres
    float shutterSpeedSeconds = 0.0f; // seconds, e.g. 0.004
    QDateTime dateTaken;              // invalid when absent
    int width = 0;                    // active area, never the raw sensor size
    int height = 0;
};

// Key/value rows extracted from RAW EXIF via LibRaw, formatted for display.
struct ImageMetadata {
    QVector<QPair<QString, QString>> rows;

    bool empty() const { return rows.isEmpty(); }
};

ExifData extractExifData(const LibRaw& raw);
ImageMetadata extractMetadata(const LibRaw& raw);

// Stable machine keys for `arraw info --json` (docs/adr/0050: JSON keys are
// identifiers, never localised display strings). Fields the file doesn't carry
// are omitted rather than emitted as zero, so a format with no EXIF at all
// (JPEG/PNG via StandardImageLoader) yields an empty object.
QJsonObject toJson(const ExifData& exif);

QJsonDocument toJson(const ImageMetadata& meta);
ImageMetadata fromJson(const QJsonDocument& doc);
