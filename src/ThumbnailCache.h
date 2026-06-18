#pragma once
#include "ImageMetadata.h"

#include <optional>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>

// Disk cache under ~/.arraw/cache/ keyed by path + file size + mtime.
// Stores up-to-512px JPEG thumbnails and JSON EXIF metadata sidecars.
class ThumbnailCache : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailCache(QObject* parent = nullptr);

    // Returns immediately when cached; otherwise generates on a worker thread.
    void request(const QString& rawPath);

    static QImage loadFromDisk(const QString& rawPath);
    static QString cachePathFor(const QString& rawPath);

    // Overwrite the cache entry for rawPath with a developed thumbnail (one that
    // reflects the current edits), replacing the camera-embedded thumbnail.
    static bool store(const QString& rawPath, const QImage& image);

    static bool storeMetadata(const QString& rawPath, const ImageMetadata& metadata);
    static std::optional<ImageMetadata> loadMetadata(const QString& rawPath);

signals:
    void thumbnailReady(const QString& rawPath, const QImage& image);

private:
    QString cacheRoot;
    QSet<QString> pending;
};
