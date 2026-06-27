#include "core/ImageMetadata.h"
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <libraw/libraw.h>
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

TEST_CASE("extractMetadata includes stable extended EXIF rows", "[metadata]") {
    LibRaw raw;
    raw.imgdata.makernotes.common.FlashEC = 1.5f;
    raw.imgdata.makernotes.common.ColorSpace = 1;
    std::strncpy(
        raw.imgdata.makernotes.common.firmware,
        "2.10",
        sizeof(raw.imgdata.makernotes.common.firmware) - 1);
    raw.imgdata.shootinginfo.DriveMode = 2;
    raw.imgdata.shootinginfo.FocusMode = 3;
    raw.imgdata.shootinginfo.ExposureMode = 1;
    std::strncpy(
        raw.imgdata.shootinginfo.InternalBodySerial,
        "internal-serial",
        sizeof(raw.imgdata.shootinginfo.InternalBodySerial) - 1);
    raw.imgdata.other.shot_order = 42;

    const ImageMetadata metadata = extractMetadata(raw);

    auto valueFor = [&](const QString& label) {
        for (const auto& [rowLabel, value] : metadata.rows) {
            if (rowLabel == label)
                return value;
        }
        return QString();
    };

    CHECK(valueFor("Flash exposure compensation") == "1.50 EV");
    CHECK(valueFor("Color space") == "sRGB");
    CHECK(valueFor("Firmware") == "2.10");
    CHECK(valueFor("Drive mode") == "Continuous high");
    CHECK(valueFor("Focus mode") == "AF-C");
    CHECK(valueFor("Exposure mode") == "Manual exposure");
    CHECK(valueFor("Internal body serial") == "internal-serial");
    CHECK(valueFor("Shot order") == "42");
}
