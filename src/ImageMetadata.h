#pragma once
#include <QPair>
#include <QString>
#include <QVector>

class LibRaw;

// Key/value rows extracted from RAW EXIF via LibRaw.
struct ImageMetadata {
    QVector<QPair<QString, QString>> rows;

    bool empty() const { return rows.isEmpty(); }
};

ImageMetadata extractMetadata(const LibRaw& raw);
