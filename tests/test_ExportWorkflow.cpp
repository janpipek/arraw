#include "ExportWorkflow.h"

#include "ExportMetadata.h"
#include "develop/UserMetadata.h"

#include <catch2/catch_test_macros.hpp>

#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

TEST_CASE("exportFormatSpec maps formats to suffixes and save filters", "[export]") {
    CHECK(exportFormatSpec(ExportOptions::Format::JPEG).suffix == "jpg");
    CHECK(exportFormatSpec(ExportOptions::Format::JPEG).nameFilter == "JPEG (*.jpg *.jpeg)");
    CHECK(std::string(exportFormatSpec(ExportOptions::Format::JPEG).saveFormat) == "JPEG");

    CHECK(exportFormatSpec(ExportOptions::Format::PNG).suffix == "png");
    CHECK(exportFormatSpec(ExportOptions::Format::PNG).nameFilter == "PNG (*.png)");
    CHECK(exportFormatSpec(ExportOptions::Format::PNG).saveFormat == nullptr);

    CHECK(exportFormatSpec(ExportOptions::Format::TIFF).suffix == "tif");
    CHECK(exportFormatSpec(ExportOptions::Format::TIFF).nameFilter == "TIFF (*.tif *.tiff)");
    CHECK(exportFormatSpec(ExportOptions::Format::TIFF).saveFormat == nullptr);
}

TEST_CASE("withExportSuffix appends only when the user omitted an extension", "[export]") {
    CHECK(withExportSuffix("/tmp/out", ExportOptions::Format::JPEG) == "/tmp/out.jpg");
    CHECK(withExportSuffix("/tmp/out.custom", ExportOptions::Format::PNG) == "/tmp/out.custom");
}

TEST_CASE("batchExportPath uses the source basename and selected export suffix", "[export]") {
    CHECK(
        batchExportPath("/exports", "/raws/IMG_0001.CR3", ExportOptions::Format::TIFF)
        == "/exports/IMG_0001.tif");
}

TEST_CASE("applyUnsharpMask leaves images unchanged when sharpening is disabled", "[export]") {
    QImage image(2, 1, QImage::Format_RGB888);
    image.setPixelColor(0, 0, QColor(10, 20, 30));
    image.setPixelColor(1, 0, QColor(40, 50, 60));

    const QImage sharpened = applyUnsharpMask(image, 0);
    CHECK(sharpened == image);
}

TEST_CASE("applyUnsharpMask handles tiny images", "[export]") {
    QImage image(1, 1, QImage::Format_RGB888);
    image.setPixelColor(0, 0, QColor(10, 20, 30));

    const QImage sharpened = applyUnsharpMask(image, 50);
    REQUIRE(!sharpened.isNull());
    CHECK(sharpened.size() == image.size());
}

TEST_CASE("saveExportImage applies JPEG quality path", "[export]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    QImage image(2, 2, QImage::Format_RGB888);
    image.fill(Qt::white);

    ExportOptions options;
    options.format = ExportOptions::Format::JPEG;
    options.quality = 80;

    const QString path = dir.filePath("out.jpg");
    REQUIRE(saveExportImage(image, path, options));
    CHECK(QFileInfo(path).isFile());
}

TEST_CASE("runExportTail renders, encodes and writes a file", "[export]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    QImage linear(4, 4, QImage::Format_RGBX32FPx4);
    linear.fill(QColor(128, 128, 128));

    ExportOptions options;
    options.format = ExportOptions::Format::JPEG;

    const QString out = dir.filePath("out.jpg");
    const ExportTailResult result = runExportTail(linear, options, out, QString(), UserMetadata{});

    CHECK(result.saved);
    CHECK(QFileInfo(out).isFile());
}

TEST_CASE("runExportTail reports failure when the write fails", "[export]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    QImage linear(4, 4, QImage::Format_RGBX32FPx4);
    linear.fill(QColor(128, 128, 128));

    ExportOptions options;
    options.format = ExportOptions::Format::JPEG;

    // Parent directory does not exist, so the encode/write cannot succeed.
    const QString out = dir.filePath("missing-subdir/out.jpg");
    const ExportTailResult result = runExportTail(linear, options, out, QString(), UserMetadata{});

    CHECK_FALSE(result.saved);
    CHECK_FALSE(QFileInfo(out).exists());
}
