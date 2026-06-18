#include "ThumbnailCache.h"
#include <catch2/catch_test_macros.hpp>

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
