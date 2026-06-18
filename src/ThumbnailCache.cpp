#include "ThumbnailCache.h"
#include "RawProcessor.h"
#include <libraw/libraw.h>
#include <memory>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrent>

namespace {

constexpr int kMaxThumbPx = 512;
constexpr int kJpegQuality = 85;

// Size and mtime are part of the key, so a modified file simply hashes to a
// new entry — stale thumbnails never need explicit invalidation.
QString cacheKey(const QFileInfo& fi) {
    QByteArray data = fi.canonicalFilePath().toUtf8();
    data += '|';
    data += QByteArray::number(fi.size());
    data += '|';
    data += QByteArray::number(fi.lastModified().toMSecsSinceEpoch());
    return QString(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString cachePath(const QString& root, const QString& key, const QString& suffix) {
    return root + "/" + key.left(2) + "/" + key + suffix;
}

QString thumbnailPath(const QString& root, const QString& key) {
    return cachePath(root, key, ".jpg");
}

QString metadataPath(const QString& root, const QString& key) {
    return cachePath(root, key, ".json");
}

QString cacheRoot() {
    const QByteArray override = qgetenv("ARRAW_CACHE_DIR");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.arraw/cache";
}

QString cachePathFor(const QString& rawPath, const QString& suffix) {
    const QFileInfo fi(rawPath);
    if (!fi.exists())
        return {};
    const QString key = cacheKey(fi);
    if (suffix == ".json")
        return metadataPath(cacheRoot(), key);
    return thumbnailPath(cacheRoot(), key);
}

QImage scaleDown(const QImage& src, int maxPx) {
    if (src.isNull())
        return {};
    if (src.width() <= maxPx && src.height() <= maxPx)
        return src;
    return src.scaled(maxPx, maxPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QImage decodeEmbeddedThumb(const QString& path) {
    auto raw = std::make_unique<LibRaw>();
    if (raw->open_file(path.toLocal8Bit().constData()) == LIBRAW_SUCCESS) {
        ThumbnailCache::storeMetadata(path, extractMetadata(*raw));
        const QImage img = RawProcessor::extractThumbImage(*raw);
        if (!img.isNull())
            return scaleDown(img, kMaxThumbPx);
    }

    // Fallback for standard image formats (JPEG, PNG, TIFF, etc.)
    // TODO: non-RAW EXIF (exiv2)
    return scaleDown(QImage(path), kMaxThumbPx);
}

bool saveJpeg(const QImage& img, const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    return img.save(path, "JPEG", kJpegQuality);
}

} // namespace

ThumbnailCache::ThumbnailCache(QObject* parent)
    : QObject(parent) {
    cacheRoot = ::cacheRoot();
    QDir().mkpath(cacheRoot);
}

QString ThumbnailCache::cachePathFor(const QString& rawPath) {
    return ::cachePathFor(rawPath, ".jpg");
}

bool ThumbnailCache::store(const QString& rawPath, const QImage& image) {
    const QString outPath = cachePathFor(rawPath);
    if (outPath.isEmpty() || image.isNull())
        return false;
    return saveJpeg(scaleDown(image, kMaxThumbPx), outPath);
}

QImage ThumbnailCache::loadFromDisk(const QString& rawPath) {
    const QString path = cachePathFor(rawPath);
    if (path.isEmpty() || !QFile::exists(path))
        return {};
    QImage img(path);
    return img.isNull() ? QImage{} : img;
}

bool ThumbnailCache::storeMetadata(const QString& rawPath, const ImageMetadata& metadata) {
    const QString outPath = ::cachePathFor(rawPath, ".json");
    if (outPath.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(outPath).absolutePath());
    QSaveFile file(outPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(toJson(metadata).toJson(QJsonDocument::Compact)) < 0)
        return false;
    return file.commit();
}

std::optional<ImageMetadata> ThumbnailCache::loadMetadata(const QString& rawPath) {
    const QString path = ::cachePathFor(rawPath, ".json");
    if (path.isEmpty() || !QFile::exists(path))
        return std::nullopt;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    return fromJson(doc);
}

void ThumbnailCache::request(const QString& rawPath) {
    if (rawPath.isEmpty())
        return;

    if (QImage cached = loadFromDisk(rawPath); !cached.isNull()) {
        emit thumbnailReady(rawPath, cached);
        return;
    }

    if (pending.contains(rawPath))
        return;
    pending.insert(rawPath);

    (void) QtConcurrent::run([this, rawPath]() {
        QImage img = decodeEmbeddedThumb(rawPath);
        const QString outPath = cachePathFor(rawPath);
        if (!img.isNull() && !outPath.isEmpty())
            saveJpeg(img, outPath);

        QMetaObject::invokeMethod(
            this,
            [this, rawPath, img = std::move(img)]() {
                pending.remove(rawPath);
                if (!img.isNull())
                    emit thumbnailReady(rawPath, img);
            },
            Qt::QueuedConnection);
    });
}
