#include "ExportMetadata.h"

#include <catch2/catch_test_macros.hpp>

#ifdef ARRAW_HAS_EXIV2
#include <exiv2/exiv2.hpp>
#endif

#include <array>
#include <QBuffer>
#include <QColorSpace>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#ifdef ARRAW_HAS_EXIV2
namespace {

void writeImage(const QString& path) {
    QImage image(4, 3, QImage::Format_RGB888);
    image.fill(QColor(20, 40, 60));
    REQUIRE(image.save(path));
}

void writeImageAs(const QString& path, const char* format) {
    QImage image(4, 3, QImage::Format_RGB888);
    image.fill(QColor(20, 40, 60));
    REQUIRE(image.save(path, format));
}

void writeDisplayP3Image(const QString& path) {
    QImage image(4, 3, QImage::Format_RGB888);
    image.fill(QColor(20, 40, 60));
    image.setColorSpace(QColorSpace::DisplayP3);
    REQUIRE(image.save(path, "JPEG"));
}

Exiv2::Image::UniquePtr readMetadata(const QString& path) {
    auto image = Exiv2::ImageFactory::open(path.toStdString());
    REQUIRE(image);
    image->readMetadata();
    return image;
}

void writeSourceExif(const QString& path) {
    auto image = readMetadata(path);
    Exiv2::ExifData& exif = image->exifData();
    exif["Exif.Image.Make"] = "Fujifilm";
    exif["Exif.Image.Model"] = "X-T5";
    exif["Exif.Image.Orientation"] = uint16_t(6);
    exif["Exif.Photo.DateTimeOriginal"] = "2026:06:28 12:34:56";
    exif["Exif.Photo.ExposureTime"] = "1/125";
    exif["Exif.Photo.FNumber"] = "8/1";
    exif["Exif.Photo.LensModel"] = "XF 35mm F2";
    exif["Exif.GPSInfo.GPSLatitudeRef"] = "N";
    exif["Exif.Image.ImageWidth"] = uint32_t(9999);
    exif["Exif.Image.ImageLength"] = uint32_t(8888);
    exif["Exif.Photo.PixelXDimension"] = uint32_t(7777);
    exif["Exif.Photo.PixelYDimension"] = uint32_t(6666);

    QByteArray thumbnailBytes;
    QBuffer thumbnailBuffer(&thumbnailBytes);
    REQUIRE(thumbnailBuffer.open(QIODevice::WriteOnly));
    QImage thumbnail(2, 2, QImage::Format_RGB888);
    thumbnail.fill(Qt::white);
    REQUIRE(thumbnail.save(&thumbnailBuffer, "JPEG"));
    Exiv2::ExifThumb(exif).setJpegThumbnail(
        reinterpret_cast<const Exiv2::byte*>(thumbnailBytes.constData()),
        size_t(thumbnailBytes.size()));
    image->writeMetadata();
}

std::string xmpText(const Exiv2::XmpData& data, const std::string& key) {
    const auto pos = data.findKey(Exiv2::XmpKey(key));
    return pos == data.end() ? std::string{} : pos->toString();
}

std::string exifText(const Exiv2::ExifData& data, const std::string& key) {
    const auto pos = data.findKey(Exiv2::ExifKey(key));
    return pos == data.end() ? std::string{} : pos->toString();
}

} // namespace
#endif

TEST_CASE("export metadata embeds descriptive User Metadata as XMP", "[export][metadata]") {
#ifndef ARRAW_HAS_EXIV2
    SKIP("exiv2 not available");
#else
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString sourcePath = dir.filePath("source.jpg");
    const QString outputPath = dir.filePath("output.jpg");
    writeImage(sourcePath);
    writeImage(outputPath);

    UserMetadata metadata;
    metadata.rating = 4;
    metadata.label = ColourLabel::Green;
    metadata.title = "Spring delivery";
    metadata.caption = "Corrected colour export";
    metadata.keywords = {"client", "print"};
    metadata.creator = "Arraw Tester";
    metadata.copyright = "Copyright 2026";

    const ExportMetadataResult result
        = embedExportMetadata(outputPath, sourcePath, metadata, ExportMetadataSelection{});
    REQUIRE(result.status == ExportMetadataStatus::Embedded);

    const auto exported = readMetadata(outputPath);
    const Exiv2::XmpData& xmp = exported->xmpData();
    CHECK(xmpText(xmp, "Xmp.dc.title") == "lang=\"x-default\" Spring delivery");
    CHECK(xmpText(xmp, "Xmp.dc.description") == "lang=\"x-default\" Corrected colour export");
    CHECK(xmpText(xmp, "Xmp.dc.creator") == "Arraw Tester");
    CHECK(xmpText(xmp, "Xmp.dc.rights") == "lang=\"x-default\" Copyright 2026");
    CHECK(xmpText(xmp, "Xmp.xmp.Rating") == "4");
    CHECK(xmpText(xmp, "Xmp.xmp.Label") == "Green");

    const auto subject = xmp.findKey(Exiv2::XmpKey("Xmp.dc.subject"));
    REQUIRE(subject != xmp.end());
    const std::string subjectText = subject->toString();
    CHECK(subjectText.find("client") != std::string::npos);
    CHECK(subjectText.find("print") != std::string::npos);
#endif
}

TEST_CASE("export metadata copies capture EXIF and corrects Orientation", "[export][metadata]") {
#ifndef ARRAW_HAS_EXIV2
    SKIP("exiv2 not available");
#else
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString sourcePath = dir.filePath("source.jpg");
    const QString outputPath = dir.filePath("output.jpg");
    writeImage(sourcePath);
    writeImage(outputPath);
    writeSourceExif(sourcePath);

    const ExportMetadataResult result
        = embedExportMetadata(outputPath, sourcePath, UserMetadata{}, ExportMetadataSelection{});
    REQUIRE(result.status == ExportMetadataStatus::Embedded);

    const auto exported = readMetadata(outputPath);
    const Exiv2::ExifData& exif = exported->exifData();
    CHECK(exifText(exif, "Exif.Image.Make") == "Fujifilm");
    CHECK(exifText(exif, "Exif.Image.Model") == "X-T5");
    CHECK(exifText(exif, "Exif.Image.Orientation") == "1");
    CHECK(exifText(exif, "Exif.Photo.DateTimeOriginal") == "2026:06:28 12:34:56");
    CHECK(exifText(exif, "Exif.Photo.ExposureTime") == "1/125");
    CHECK(exifText(exif, "Exif.Photo.FNumber") == "8/1");
    CHECK(exifText(exif, "Exif.Photo.LensModel") == "XF 35mm F2");
    CHECK(exifText(exif, "Exif.GPSInfo.GPSLatitudeRef").empty());
    CHECK(exifText(exif, "Exif.Image.ImageWidth").empty());
    CHECK(exifText(exif, "Exif.Image.ImageLength").empty());
    CHECK(exifText(exif, "Exif.Photo.PixelXDimension").empty());
    CHECK(exifText(exif, "Exif.Photo.PixelYDimension").empty());
    CHECK(exifText(exif, "Exif.Image.Software") == "arraw");
    CHECK(Exiv2::ExifThumbC(exif).copy().empty());
#endif
}

TEST_CASE(
    "export metadata selection gates capture location and descriptive groups",
    "[export][metadata]") {
#ifndef ARRAW_HAS_EXIV2
    SKIP("exiv2 not available");
#else
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString sourcePath = dir.filePath("source.jpg");
    const QString outputPath = dir.filePath("output.jpg");
    writeImage(sourcePath);
    writeImage(outputPath);
    writeSourceExif(sourcePath);

    UserMetadata metadata;
    metadata.title = "Private title";

    ExportMetadataSelection none;
    none.includeCaptureInfo = false;
    none.includeLocation = false;
    none.includeDescriptive = false;
    REQUIRE(
        embedExportMetadata(outputPath, sourcePath, metadata, none).status
        == ExportMetadataStatus::Embedded);

    auto exported = readMetadata(outputPath);
    CHECK(exifText(exported->exifData(), "Exif.Image.Make").empty());
    CHECK(exifText(exported->exifData(), "Exif.GPSInfo.GPSLatitudeRef").empty());
    CHECK(xmpText(exported->xmpData(), "Xmp.dc.title").empty());

    const QString gpsOnlyPath = dir.filePath("gps-only.jpg");
    writeImage(gpsOnlyPath);
    ExportMetadataSelection gpsOnly;
    gpsOnly.includeCaptureInfo = false;
    gpsOnly.includeLocation = true;
    gpsOnly.includeDescriptive = false;
    REQUIRE(
        embedExportMetadata(gpsOnlyPath, sourcePath, metadata, gpsOnly).status
        == ExportMetadataStatus::Embedded);

    exported = readMetadata(gpsOnlyPath);
    CHECK(exifText(exported->exifData(), "Exif.Image.Make").empty());
    CHECK(exifText(exported->exifData(), "Exif.GPSInfo.GPSLatitudeRef") == "N");
    CHECK(xmpText(exported->xmpData(), "Xmp.dc.title").empty());
#endif
}

TEST_CASE("export metadata preserves the ICC profile written by Qt", "[export][metadata]") {
#ifndef ARRAW_HAS_EXIV2
    SKIP("exiv2 not available");
#else
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString sourcePath = dir.filePath("source.jpg");
    const QString outputPath = dir.filePath("output.jpg");
    writeImage(sourcePath);
    writeDisplayP3Image(outputPath);
    REQUIRE(readMetadata(outputPath)->iccProfileDefined());

    UserMetadata metadata;
    metadata.title = "Profiled export";
    const ExportMetadataResult result
        = embedExportMetadata(outputPath, sourcePath, metadata, ExportMetadataSelection{});
    REQUIRE(result.status == ExportMetadataStatus::Embedded);

    const auto exported = readMetadata(outputPath);
    CHECK(exported->iccProfileDefined());
    CHECK(xmpText(exported->xmpData(), "Xmp.dc.title") == "lang=\"x-default\" Profiled export");
#endif
}

TEST_CASE("export metadata failure leaves the encoded image intact", "[export][metadata]") {
#ifndef ARRAW_HAS_EXIV2
    SKIP("exiv2 not available");
#else
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString outputPath = dir.filePath("output.jpg");
    writeImage(outputPath);

    const ExportMetadataResult result = embedExportMetadata(
        outputPath, dir.filePath("missing-source.jpg"), UserMetadata{}, ExportMetadataSelection{});
    CHECK(result.status == ExportMetadataStatus::Failed);
    CHECK(!result.error.isEmpty());
    CHECK(QFileInfo(outputPath).isFile());
    CHECK(!QImage(outputPath).isNull());
#endif
}

TEST_CASE("export metadata embeds into JPEG TIFF and PNG outputs", "[export][metadata]") {
#ifndef ARRAW_HAS_EXIV2
    SKIP("exiv2 not available");
#else
    struct FormatCase {
        const char* suffix;
        const char* writerFormat;
    };

    constexpr std::array formats = {
        FormatCase{"jpg", "JPEG"},
        FormatCase{"tif", "TIFF"},
        FormatCase{"png", "PNG"},
    };

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString sourcePath = dir.filePath("source.jpg");
    writeImage(sourcePath);
    writeSourceExif(sourcePath);

    for (const FormatCase& format : formats) {
        INFO(format.suffix);
        const QString outputPath = dir.filePath(QString("output.") + format.suffix);
        writeImageAs(outputPath, format.writerFormat);

        UserMetadata metadata;
        metadata.title = QString("Format ") + format.suffix;
        const ExportMetadataResult result
            = embedExportMetadata(outputPath, sourcePath, metadata, ExportMetadataSelection{});
        REQUIRE(result.status == ExportMetadataStatus::Embedded);

        const auto exported = readMetadata(outputPath);
        CHECK(exifText(exported->exifData(), "Exif.Image.Orientation") == "1");
        CHECK(
            xmpText(exported->xmpData(), "Xmp.dc.title")
            == std::string("lang=\"x-default\" Format ") + format.suffix);
    }
#endif
}

TEST_CASE("export metadata reports skipped when exiv2 backend is unavailable", "[export][metadata]") {
#ifdef ARRAW_HAS_EXIV2
    SKIP("exiv2 backend is available");
#else
    const ExportMetadataResult result
        = embedExportMetadata({}, {}, UserMetadata{}, ExportMetadataSelection{});
    CHECK(result.status == ExportMetadataStatus::SkippedNoBackend);
    CHECK(result.ok());
#endif
}
