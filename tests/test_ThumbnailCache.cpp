#include "ThumbnailCache.h"
#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QFile>
#include <QTemporaryDir>

TEST_CASE("ThumbnailCache stores metadata sidecars keyed by file identity", "[thumbcache]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    qputenv("ARRAW_CACHE_DIR", dir.filePath("cache").toLocal8Bit());

    const QString rawPath = dir.filePath("frame.dng");
    QFile raw(rawPath);
    REQUIRE(raw.open(QIODevice::WriteOnly));
    REQUIRE(raw.write("raw") == 3);
    raw.close();

    ImageMetadata meta;
    meta.rows.append(qMakePair(QString("Make"), QString("Fujifilm")));
    meta.rows.append(qMakePair(QString("Model"), QString("X-T5")));

    const QString firstPath = ThumbnailCache::cachePathFor(rawPath);
    REQUIRE(ThumbnailCache::storeMetadata(rawPath, meta));

    const std::optional<ImageMetadata> loaded = ThumbnailCache::loadMetadata(rawPath);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->rows == meta.rows);

    REQUIRE(raw.open(QIODevice::WriteOnly | QIODevice::Append));
    REQUIRE(raw.write(" changed") == 8);
    raw.close();

    CHECK_FALSE(ThumbnailCache::loadMetadata(rawPath).has_value());

    QString firstMetadataPath = firstPath;
    firstMetadataPath.replace(".jpg", ".json");
    QString secondMetadataPath = ThumbnailCache::cachePathFor(rawPath);
    secondMetadataPath.replace(".jpg", ".json");
    QFile::remove(firstMetadataPath);
    QFile::remove(secondMetadataPath);
    qunsetenv("ARRAW_CACHE_DIR");
}

TEST_CASE(
    "ThumbnailCache rejects stale embedded writes after developed thumbnail store", "[thumbcache]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    qputenv("ARRAW_CACHE_DIR", dir.filePath("cache").toLocal8Bit());

    const QString rawPath = dir.filePath("frame.dng");
    QFile raw(rawPath);
    REQUIRE(raw.open(QIODevice::WriteOnly));
    REQUIRE(raw.write("raw") == 3);
    raw.close();

    const quint64 embeddedGeneration = ThumbnailCache::cacheGenerationForTesting(rawPath);

    QImage developed(16, 16, QImage::Format_RGB32);
    developed.fill(QColor(220, 10, 10));
    REQUIRE(ThumbnailCache::store(rawPath, developed));
    CHECK(ThumbnailCache::cacheGenerationForTesting(rawPath) == embeddedGeneration + 1);

    QImage embedded(16, 16, QImage::Format_RGB32);
    embedded.fill(QColor(10, 10, 220));
    CHECK_FALSE(
        ThumbnailCache::storeIfGenerationMatchesForTesting(rawPath, embedded, embeddedGeneration));

    const QImage cached = ThumbnailCache::loadFromDisk(rawPath);
    REQUIRE_FALSE(cached.isNull());
    const QColor pixel(cached.pixel(0, 0));
    CHECK(pixel.red() > pixel.blue());

    QFile::remove(ThumbnailCache::cachePathFor(rawPath));
    qunsetenv("ARRAW_CACHE_DIR");
}
