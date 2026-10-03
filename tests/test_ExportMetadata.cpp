#include "MetadataEmbedding.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"
#include "support/TestImages.h"

#include <Diagnostics.h>
#include <ExifInfo.h>
#include <ImageExport.h>
#include <ImageImport.h>

#include <QByteArray>
#include <QColorSpace>
#include <QImage>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <exiv2/exiv2.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace arraw;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

namespace fs = std::filesystem;

constexpr ImageSize outputSize{20, 12};

/// @brief Opens a file with exiv2 and reads its metadata.
Exiv2::Image::UniquePtr open(const fs::path& path) {
    auto image = Exiv2::ImageFactory::open(path.string());
    image->readMetadata();
    return image;
}

/// @brief The values `exiv2` reads for a tag, or an empty string.
std::string tag(const Exiv2::Image& image, const char* key) {
    const auto found = image.exifData().findKey(Exiv2::ExifKey(key));
    return found == image.exifData().end() ? std::string() : found->toString();
}

/// @brief The text of an XMP property, or an empty string.
std::string property(const Exiv2::Image& image, const char* key) {
    const auto found = image.xmpData().findKey(Exiv2::XmpKey(key));
    return found == image.xmpData().end() ? std::string() : found->toString();
}

bool hasGroup(const Exiv2::Image& image, const std::string& group) {
    for (const auto& datum : image.exifData()) {
        if (datum.groupName() == group) {
            return true;
        }
    }
    return false;
}

fs::path extensionOf(const test::TempDir& directory, ImageFileFormat format, const char* stem) {
    switch (format) {
    case ImageFileFormat::Jpeg:
        return directory.file(std::string(stem) + ".jpg");
    case ImageFileFormat::Png:
        return directory.file(std::string(stem) + ".png");
    case ImageFileFormat::Tiff:
        return directory.file(std::string(stem) + ".tif");
    }
    return {};
}

std::vector<char> bytesOf(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

/// @brief Copies the EXIF fixture into a directory, so a sidecar can sit beside it.
fs::path copiedFixture(const test::TempDir& directory) {
    const auto copy = directory.file("shot.dng");
    fs::copy_file(test::fixture("exif-32x24.dng"), copy);
    return copy;
}

/// @brief Writes a JPEG source with a MakerNote, a thumbnail, serial numbers and XMP.
fs::path richJpeg(const test::TempDir& directory) {
    const auto path = directory.file("rich.jpg");
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::gray);
    REQUIRE(image.save(QString::fromStdString(path.string())));
    auto file = Exiv2::ImageFactory::open(path.string());
    file->readMetadata();
    auto& exif = file->exifData();
    exif["Exif.Image.Make"] = "Arraw";
    exif["Exif.Image.Orientation"] = std::uint16_t{6};
    exif["Exif.Photo.BodySerialNumber"] = "SN-1234";
    exif["Exif.Photo.MakerNote"] = "opaque maker bytes";
    exif["Exif.Photo.PixelXDimension"] = std::uint32_t{4000};
    Exiv2::ExifThumb thumb(exif);
    const std::vector<Exiv2::byte> tiny{0xFF, 0xD8, 0xFF, 0xD9};
    thumb.setJpegThumbnail(tiny.data(), tiny.size());
    file->xmpData()["Xmp.dc.title"] = "lang=\"x-default\" From the source";
    file->xmpData()["Xmp.dc.rights"] = "lang=\"x-default\" Source rights";
    file->writeMetadata();
    return path;
}

} // namespace

TEST_CASE("Capture metadata is carried by default, and location is not",
          "[ExportMetadata][integration]") {
    const auto format =
        GENERATE(ImageFileFormat::Jpeg, ImageFileFormat::Tiff, ImageFileFormat::Png);
    CAPTURE(format);
    const test::TempDir directory;
    const auto destination = extensionOf(directory, format, "out");

    exportImage(test::rainbow(outputSize), destination, {},
                ExportMetadata{.source = test::fixture("exif-32x24.dng")});

    const ExifInfo info = readExif(destination);
    REQUIRE(info.make == "Arraw");
    REQUIRE(info.model == "Fixture One");
    REQUIRE(info.lensModel == "Fixture 35mm F2.8");
    REQUIRE(info.dateTimeOriginal == "2024:05:01 10:00:00");
    REQUIRE(info.offsetTimeOriginal == "+02:00");
    REQUIRE(info.exposureTime == URational{1, 250});
    REQUIRE(info.fNumber == URational{28, 10});
    REQUIRE(info.photographicSensitivity == 400);
    REQUIRE(info.focalLength == URational{35, 1});
    REQUIRE(info.focalLengthIn35mmFilm == 52);
    REQUIRE(info.exposureBiasValue == SRational{-1, 3});
    REQUIRE(info.flash == 16);
    REQUIRE_FALSE(info.gps);
    // Descriptive is on by default, and the fixture's Artist and Copyright are of it.
    REQUIRE(info.artist == "Ada Lovelace");

    const auto written = open(destination);
    REQUIRE(tag(*written, "Exif.Image.Orientation") == "1");
    REQUIRE(tag(*written, "Exif.Photo.PixelXDimension") == "20");
    REQUIRE(tag(*written, "Exif.Photo.PixelYDimension") == "12");
    REQUIRE(tag(*written, "Exif.Photo.ColorSpace") == "1");
    REQUIRE_THAT(tag(*written, "Exif.Image.Software"), ContainsSubstring("arraw "));
    REQUIRE_FALSE(hasGroup(*written, "GPSInfo"));
    REQUIRE(property(*written, "Xmp.exif.GPSLatitude").empty());
}

TEST_CASE("Location adds the GPS tags", "[ExportMetadata][integration]") {
    const auto format =
        GENERATE(ImageFileFormat::Jpeg, ImageFileFormat::Tiff, ImageFileFormat::Png);
    CAPTURE(format);
    const test::TempDir directory;
    const auto destination = extensionOf(directory, format, "out");

    exportImage(
        test::rainbow(outputSize), destination, {},
        ExportMetadata{.source = test::fixture("exif-32x24.dng"),
                       .selection = {.capture = false, .location = true, .descriptive = false}});

    const ExifInfo info = readExif(destination);
    REQUIRE(info.gps);
    REQUIRE(info.gps->latitude == Approx(50.0 + 5.0 / 60.0 + 15.75 / 3600.0));
    REQUIRE(info.gps->longitude == Approx(14.0 + 25.0 / 60.0 + 18.0 / 3600.0));
    REQUIRE(info.gps->altitude == Approx(235.5));
    REQUIRE_FALSE(info.make);
    REQUIRE_FALSE(info.exposureTime);
}

TEST_CASE("Descriptive metadata writes the marks, and the sidecar beats the source",
          "[ExportMetadata][integration]") {
    const auto format =
        GENERATE(ImageFileFormat::Jpeg, ImageFileFormat::Tiff, ImageFileFormat::Png);
    CAPTURE(format);
    const test::TempDir directory;
    const auto source = richJpeg(directory);
    {
        std::ofstream sidecar(directory.file("rich.xmp"));
        sidecar << R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/">
   <xmp:Rating>2</xmp:Rating>
   <dc:title><rdf:Alt><rdf:li xml:lang="x-default">From the sidecar</rdf:li></rdf:Alt></dc:title>
   <dc:subject><rdf:Bag><rdf:li>street</rdf:li><rdf:li>night</rdf:li></rdf:Bag></dc:subject>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
)";
    }
    const auto destination = extensionOf(directory, format, "out");

    exportImage(
        test::rainbow(outputSize), destination, {},
        ExportMetadata{.source = source,
                       .marks = {.rating = -1, .label = ColorLabel::Green},
                       .selection = {.capture = false, .location = false, .descriptive = true}});

    const auto written = open(destination);
    // The marks decide the rating, not the sidecar's own xmp:Rating.
    REQUIRE(property(*written, "Xmp.xmp.Rating") == "-1");
    REQUIRE(property(*written, "Xmp.xmp.Label") == "Green");
    REQUIRE_THAT(property(*written, "Xmp.dc.title"), ContainsSubstring("From the sidecar"));
    REQUIRE_THAT(property(*written, "Xmp.dc.subject"), ContainsSubstring("night"));
    // Not in the sidecar, so the source's own stays.
    REQUIRE_THAT(property(*written, "Xmp.dc.rights"), ContainsSubstring("Source rights"));
    // Capture was not chosen.
    REQUIRE_FALSE(readExif(destination).make);
}

TEST_CASE("No stars and no label write no rating and no label", "[ExportMetadata][integration]") {
    const test::TempDir directory;
    const auto destination = directory.file("out.jpg");
    exportImage(
        test::rainbow(outputSize), destination, {},
        ExportMetadata{.source = richJpeg(directory),
                       .selection = {.capture = false, .location = false, .descriptive = true}});
    const auto written = open(destination);
    REQUIRE(property(*written, "Xmp.xmp.Rating").empty());
    REQUIRE(property(*written, "Xmp.xmp.Label").empty());
    REQUIRE_THAT(property(*written, "Xmp.dc.title"), ContainsSubstring("From the source"));
}

TEST_CASE("A selection of nothing is bit-identical to no metadata", "[ExportMetadata]") {
    const auto format =
        GENERATE(ImageFileFormat::Jpeg, ImageFileFormat::Tiff, ImageFileFormat::Png);
    const test::TempDir directory;
    const auto plain = extensionOf(directory, format, "plain");
    const auto none = extensionOf(directory, format, "none");
    exportImage(test::rainbow(outputSize), plain, {});
    exportImage(test::rainbow(outputSize), none, {},
                ExportMetadata{.source = test::fixture("exif-32x24.dng"),
                               .marks = {.rating = 3},
                               .selection = {false, false, false}});
    REQUIRE(bytesOf(plain) == bytesOf(none));
}

TEST_CASE("A source with nothing to carry writes nothing", "[ExportMetadata][integration]") {
    const test::TempDir directory;
    const auto plain = directory.file("plain.jpg");
    const auto out = directory.file("out.jpg");
    exportImage(test::rainbow(outputSize), plain, {});
    exportImage(test::rainbow(outputSize), out, {},
                ExportMetadata{.source = test::fixture("linear-32x24-neutral.dng"),
                               .selection = {true, true, true}});
    REQUIRE(bytesOf(plain) == bytesOf(out));
}

TEST_CASE("Maker notes, thumbnails and serial numbers are never carried",
          "[ExportMetadata][integration]") {
    const auto format =
        GENERATE(ImageFileFormat::Jpeg, ImageFileFormat::Tiff, ImageFileFormat::Png);
    const test::TempDir directory;
    const auto destination = extensionOf(directory, format, "out");
    exportImage(test::rainbow(outputSize), destination, {},
                ExportMetadata{.source = richJpeg(directory), .selection = {true, true, true}});

    const auto written = open(destination);
    REQUIRE(tag(*written, "Exif.Image.Make") == "Arraw");
    REQUIRE(tag(*written, "Exif.Photo.MakerNote").empty());
    REQUIRE(tag(*written, "Exif.Photo.BodySerialNumber").empty());
    REQUIRE_FALSE(hasGroup(*written, "Thumbnail"));
    // The output's own dimensions and an upright orientation, not the source's.
    REQUIRE(tag(*written, "Exif.Image.Orientation") == "1");
    REQUIRE(tag(*written, "Exif.Photo.PixelXDimension") == "20");
    REQUIRE(Exiv2::ExifThumbC(written->exifData()).copy().size() == 0);
}

TEST_CASE("Embedding leaves the pixels and the profile alone", "[ExportMetadata][integration]") {
    const auto format =
        GENERATE(ImageFileFormat::Jpeg, ImageFileFormat::Tiff, ImageFileFormat::Png);
    const auto bitDepth = format == ImageFileFormat::Jpeg ? 8 : GENERATE(8, 16);
    CAPTURE(format, bitDepth);
    const test::TempDir directory;
    const auto plain = extensionOf(directory, format, "plain");
    const auto tagged = extensionOf(directory, format, "tagged");
    const ExportOptions options{.encoding = NamedEncoding::DisplayP3, .bitDepth = bitDepth};
    exportImage(test::rainbow({21, 13}), plain, options);
    exportImage(test::rainbow({21, 13}), tagged, options,
                ExportMetadata{.source = test::fixture("exif-32x24.dng"),
                               .marks = {.rating = 4},
                               .selection = {true, true, true}});

    const QImage before(QString::fromStdString(plain.string()));
    const QImage after(QString::fromStdString(tagged.string()));
    REQUIRE_FALSE(after.isNull());
    REQUIRE(after.size() == before.size());
    REQUIRE(after.format() == before.format());
    REQUIRE(after == before);
    REQUIRE(after.colorSpace().isValid());
    REQUIRE(after.colorSpace() == before.colorSpace());
    // Not sRGB, so EXIF is told it is uncalibrated.
    REQUIRE(tag(*open(tagged), "Exif.Photo.ColorSpace") == "65535");
}

TEST_CASE("A source that cannot be read exports without metadata and warns", "[ExportMetadata]") {
    const test::TempDir directory;
    const auto destination = directory.file("out.jpg");
    const auto plain = directory.file("plain.jpg");
    const auto garbage = directory.file("garbage.dng");
    std::ofstream(garbage) << "not an image at all";
    exportImage(test::rainbow(outputSize), plain, {});

    for (const auto& source : {directory.file("absent.dng"), garbage}) {
        CAPTURE(source);
        CollectedDiagnostics log;
        REQUIRE_NOTHROW(exportImage(test::rainbow(outputSize), destination, {},
                                    ExportMetadata{.source = source}, log));
        REQUIRE(bytesOf(destination) == bytesOf(plain));
        REQUIRE(log.entries().size() == 1);
        const auto& warning = log.entries().front();
        REQUIRE(warning.notice == Notice::MetadataNotCarried);
        REQUIRE(warning.severity == Severity::Warning);
        REQUIRE(warning.subject == source);
        REQUIRE_THAT(describe(warning), ContainsSubstring("without some metadata"));
        fs::remove(destination);
    }
}

TEST_CASE("A source that cannot be read still carries the marks", "[ExportMetadata]") {
    // The marks are arraw's own knowledge, not the source's.
    const test::TempDir directory;
    const auto destination = directory.file("out.jpg");
    CollectedDiagnostics log;
    exportImage(test::rainbow(outputSize), destination, {},
                ExportMetadata{.source = directory.file("absent.dng"),
                               .marks = {.rating = 4, .label = ColorLabel::Red}},
                log);
    REQUIRE(log.entries().size() == 1);
    const auto written = open(destination);
    REQUIRE(written->xmpData()["Xmp.xmp.Rating"].toString() == "4");
    REQUIRE(written->xmpData()["Xmp.xmp.Label"].toString() == "Red");
    REQUIRE(tag(*written, "Exif.Image.Model").empty());
}

TEST_CASE("Exports that carry nothing, or everything readable, do not warn", "[ExportMetadata]") {
    const test::TempDir directory;
    CollectedDiagnostics log;
    exportImage(test::rainbow(outputSize), directory.file("a.jpg"), {},
                ExportMetadata{.source = test::fixture("exif-32x24.dng")}, log);
    exportImage(
        test::rainbow(outputSize), directory.file("b.jpg"), {},
        ExportMetadata{.source = directory.file("absent.dng"), .selection = {false, false, false}},
        log);
    REQUIRE(log.entries().empty());
}

TEST_CASE("A sidecar that is not XMP is left out, and the capture is carried", "[ExportMetadata]") {
    const test::TempDir directory;
    const auto source = copiedFixture(directory);
    std::ofstream(directory.file("shot.xmp")) << "this is not xml";
    const auto destination = directory.file("out.png");
    CollectedDiagnostics log;
    exportImage(test::rainbow(outputSize), destination, {}, ExportMetadata{.source = source}, log);

    const auto written = open(destination);
    REQUIRE(tag(*written, "Exif.Image.Make") == "Arraw");
    REQUIRE(property(*written, "Xmp.dc.title").empty());
    REQUIRE(log.entries().size() == 1);
    const auto& warning = log.entries().front();
    REQUIRE(warning.notice == Notice::MetadataNotCarried);
    REQUIRE(warning.subject == source);
    REQUIRE_THAT(describe(warning), ContainsSubstring("shot.xmp"));
}

TEST_CASE("Metadata that cannot be written into the output is an error", "[ExportMetadata]") {
    // Not reachable through exportImage, whose own encoder makes the bytes; so the
    // embedding is given bytes exiv2 cannot take as an image.
    const QByteArray notAnImage("these bytes are no JPEG, PNG or TIFF");
    CollectedDiagnostics log;
    REQUIRE_THROWS_AS(embedMetadata(notAnImage,
                                    ExportMetadata{.source = test::fixture("exif-32x24.dng")},
                                    {20, 12, true}, log),
                      std::runtime_error);
}

TEST_CASE("A rating out of range is refused", "[ExportMetadata]") {
    const test::TempDir directory;
    REQUIRE_THROWS_AS(exportImage(test::rainbow(outputSize), directory.file("out.jpg"), {},
                                  ExportMetadata{.source = test::fixture("exif-32x24.dng"),
                                                 .marks = {.rating = 9}}),
                      std::invalid_argument);
    REQUIRE_FALSE(fs::exists(directory.file("out.jpg")));
}
