#include "core/ImageMetadata.h"
#include "ThumbnailCache.h"
#include "pipeline/RawProcessor.h"
#include <libraw/libraw.h>
#include <memory>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonParseError>
#include <QMutexLocker>
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

QMutex generationMutex;
QHash<QString, quint64> generations;

quint64 generationForCachePath(const QString& path) {
    QMutexLocker lock(&generationMutex);
    return generations.value(path);
}

bool storeNextGeneration(const QString& path, const QImage& image) {
    QMutexLocker lock(&generationMutex);
    const quint64 next = generations.value(path) + 1;
    generations.insert(path, next);
    return saveJpeg(scaleDown(image, kMaxThumbPx), path);
}

bool generationStillCurrentUnlocked(const QString& path, quint64 generation) {
    return generations.value(path) == generation;
}

bool generationStillCurrent(const QString& path, quint64 generation) {
    QMutexLocker lock(&generationMutex);
    return generationStillCurrentUnlocked(path, generation);
}

bool storeIfGenerationMatches(const QString& outPath, const QImage& image, quint64 generation) {
    if (outPath.isEmpty() || image.isNull())
        return false;
    QMutexLocker lock(&generationMutex);
    if (!generationStillCurrentUnlocked(outPath, generation))
        return false;
    return saveJpeg(scaleDown(image, kMaxThumbPx), outPath);
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
    return storeNextGeneration(outPath, image);
}

#ifdef ARRAW_TESTING
quint64 ThumbnailCache::cacheGenerationForTesting(const QString& rawPath) {
    const QString path = cachePathFor(rawPath);
    return path.isEmpty() ? 0 : generationForCachePath(path);
}

bool ThumbnailCache::storeIfGenerationMatchesForTesting(
    const QString& rawPath, const QImage& image, quint64 generation) {
    return storeIfGenerationMatches(cachePathFor(rawPath), image, generation);
}
#endif

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

    {
        QMutexLocker lock(&pendingMutex);
        if (pending.contains(rawPath))
            return;
        pending.insert(rawPath);
    }

    const QString outPath = cachePathFor(rawPath);
    const quint64 generation = outPath.isEmpty() ? 0 : generationForCachePath(outPath);

    (void) QtConcurrent::run([this, rawPath, outPath, generation]() {
        QImage img = decodeEmbeddedThumb(rawPath);
        if (!img.isNull())
            storeIfGenerationMatches(outPath, img, generation);

        QMetaObject::invokeMethod(
            this,
            [this, rawPath, outPath, generation, img = std::move(img)]() {
                {
                    QMutexLocker lock(&pendingMutex);
                    pending.remove(rawPath);
                }
                if (!img.isNull() && !outPath.isEmpty()
                    && generationStillCurrent(outPath, generation))
                    emit thumbnailReady(rawPath, img);
            },
            Qt::QueuedConnection);
    });
}
