#include "ImageMetadata.h"
#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>

TEST_CASE("ImageMetadata round-trips through stable JSON", "[metadata]") {
    ImageMetadata meta;
    meta.rows.append(qMakePair(QString("Make"), QString("Canon")));
    meta.rows.append(qMakePair(QString("Model"), QString("EOS R5")));
    meta.rows.append(qMakePair(QString("Lens"), QString("RF 50mm F1.2L USM")));
    meta.rows.append(qMakePair(QString("ISO"), QString("400")));

    const QByteArray bytes = toJson(meta).toJson(QJsonDocument::Compact);
    const ImageMetadata roundTripped = fromJson(QJsonDocument::fromJson(bytes));

    REQUIRE(roundTripped.rows == meta.rows);
    REQUIRE(toJson(roundTripped).toJson(QJsonDocument::Compact) == bytes);
}
