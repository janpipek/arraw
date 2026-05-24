#include "ThumbnailCache.h"
#include <libraw/libraw.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrent>

namespace {

constexpr int kMaxThumbPx = 512;
constexpr int kJpegQuality = 85;

QString cacheKey(const QFileInfo& fi) {
    QByteArray data = fi.canonicalFilePath().toUtf8();
    data += '|';
    data += QByteArray::number(fi.size());
    data += '|';
    data += QByteArray::number(fi.lastModified().toMSecsSinceEpoch());
    return QString(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString cachePath(const QString& root, const QString& key) {
    return root + "/" + key.left(2) + "/" + key + ".jpg";
}

QImage scaleDown(const QImage& src, int maxPx) {
    if (src.isNull())
        return {};
    if (src.width() <= maxPx && src.height() <= maxPx)
        return src;
    return src.scaled(maxPx, maxPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QImage decodeEmbeddedThumb(const QString& path) {
    LibRaw raw;
    const QByteArray local = path.toLocal8Bit();
    if (raw.open_file(local.constData()) != LIBRAW_SUCCESS)
        return {};
    if (raw.unpack_thumb() != LIBRAW_SUCCESS)
        return {};

    int err = 0;
    libraw_processed_image_t* thumb = raw.dcraw_make_mem_thumb(&err);
    if (!thumb || err != LIBRAW_SUCCESS)
        return {};

    QImage img;
    if (thumb->type == LIBRAW_IMAGE_JPEG) {
        img.loadFromData(reinterpret_cast<const uchar*>(thumb->data),
                         int(thumb->data_size), "JPEG");
    } else {
        img = QImage(thumb->width, thumb->height, QImage::Format_RGB888);
        if (!img.isNull()) {
            const int pixels = thumb->width * thumb->height * 3;
            if (thumb->bits == 8) {
                memcpy(img.bits(), thumb->data, size_t(pixels));
            } else if (thumb->bits == 16) {
                const auto* src = reinterpret_cast<const uint16_t*>(thumb->data);
                auto* dst = img.bits();
                for (int i = 0; i < pixels; ++i)
                    dst[i] = uchar(src[i] >> 8);
            }
        }
    }

    LibRaw::dcraw_clear_mem(thumb);
    return scaleDown(img, kMaxThumbPx);
}

bool saveJpeg(const QImage& img, const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    return img.save(path, "JPEG", kJpegQuality);
}

} // namespace

ThumbnailCache::ThumbnailCache(QObject* parent) : QObject(parent) {
    cacheRoot = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
              + "/.arraw/cache";
    QDir().mkpath(cacheRoot);
}

QString ThumbnailCache::cachePathFor(const QString& rawPath) {
    const QFileInfo fi(rawPath);
    if (!fi.exists())
        return {};
    const QString root = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                       + "/.arraw/cache";
    return cachePath(root, cacheKey(fi));
}

QImage ThumbnailCache::loadFromDisk(const QString& rawPath) {
    const QString path = cachePathFor(rawPath);
    if (path.isEmpty() || !QFile::exists(path))
        return {};
    QImage img(path);
    return img.isNull() ? QImage{} : img;
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

    (void)QtConcurrent::run([this, rawPath]() {
        QImage img = decodeEmbeddedThumb(rawPath);
        const QString outPath = cachePathFor(rawPath);
        if (!img.isNull() && !outPath.isEmpty())
            saveJpeg(img, outPath);

        QMetaObject::invokeMethod(this, [this, rawPath, img = std::move(img)]() {
            pending.remove(rawPath);
            if (!img.isNull())
                emit thumbnailReady(rawPath, img);
        }, Qt::QueuedConnection);
    });
}
