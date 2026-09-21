#include "ImageExport.h"

#include "support/TempDir.h"
#include "support/TestImages.h"

#include <QColor>
#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QPixelFormat>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <vector>

using namespace arraw;

namespace {

/// @brief Reads the first bytes of a file.
/// @param path File to read.
/// @param count Number of bytes wanted.
/// @return The bytes read, which may be shorter than @p count.
std::vector<std::uint8_t> head(const std::filesystem::path& path, std::size_t count) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(count);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    return bytes;
}

/// @brief Checks that a file begins with a format's magic number.
/// @param path File to inspect.
/// @param signature Bytes the format is required to start with.
/// @return `true` if the file starts with @p signature.
bool startsWith(const std::filesystem::path& path, std::initializer_list<std::uint8_t> signature) {
    return head(path, signature.size()) == std::vector<std::uint8_t>(signature);
}

constexpr std::initializer_list<std::uint8_t> pngSignature = {0x89, 'P',  'N',  'G',
                                                              0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::initializer_list<std::uint8_t> jpegSignature = {0xFF, 0xD8, 0xFF};

} // namespace

TEST_CASE("Exporting writes the format named by the extension", "[ImageExport]") {
    const test::TempDir directory;

    SECTION("PNG") {
        const auto destination = directory.file("rainbow.png");
        exportImage(test::rainbow(), destination, {});

        REQUIRE(std::filesystem::exists(destination));
        REQUIRE(std::filesystem::file_size(destination) > 0);
        REQUIRE(startsWith(destination, pngSignature));
    }

    SECTION("JPEG") {
        const auto destination = directory.file("rainbow.jpg");
        exportImage(test::rainbow(), destination, {});

        REQUIRE(std::filesystem::file_size(destination) > 0);
        REQUIRE(startsWith(destination, jpegSignature));
    }
}

TEST_CASE("A requested format overrides the extension", "[ImageExport]") {
    const test::TempDir directory;
    const auto destination = directory.file("rainbow.dat");

    ExportOptions options;
    options.format = ImageFileFormat::Png;
    exportImage(test::rainbow(), destination, options);

    REQUIRE(startsWith(destination, pngSignature));
}

TEST_CASE("An unrecognised extension is refused", "[ImageExport]") {
    const test::TempDir directory;
    const auto image = test::rainbow();

    REQUIRE_THROWS_AS(exportImage(image, directory.file("rainbow.bmp"), {}), std::invalid_argument);
    REQUIRE_THROWS_AS(exportImage(image, directory.file("rainbow"), {}), std::invalid_argument);
}

TEST_CASE("Exporting replaces an existing destination", "[ImageExport]") {
    const test::TempDir directory;
    const auto destination = directory.file("rainbow.png");
    {
        std::ofstream stream(destination, std::ios::binary);
        stream << "previous export";
        REQUIRE(stream.good());
    }

    exportImage(test::rainbow(), destination, {});

    REQUIRE(startsWith(destination, pngSignature));
}

TEST_CASE("An unsupported image preserves an existing destination", "[ImageExport]") {
    const test::TempDir directory;
    const auto destination = directory.file("rainbow.png");
    exportImage(test::rainbow(), destination, {});
    const auto original = head(destination, std::filesystem::file_size(destination));

    for (const auto format : {PixelFormat::RgbU16, PixelFormat::RgbF32}) {
        const auto image = test::rainbow({3, 2}, format);
        REQUIRE_THROWS_AS(exportImage(image, destination, {}), std::invalid_argument);
        REQUIRE(std::filesystem::file_size(destination) == original.size());
        REQUIRE(head(destination, original.size()) == original);
    }
}

TEST_CASE("An unavailable export destination reports an exception", "[ImageExport]") {
    const test::TempDir directory;
    const auto destination = directory.file("missing") / "rainbow.png";

    REQUIRE_THROWS_AS(exportImage(test::rainbow(), destination, {}), std::runtime_error);
    REQUIRE_FALSE(std::filesystem::exists(destination));
}

TEST_CASE("Lossless exports preserve odd-width pixels at the requested depth", "[ImageExport]") {
    const auto format = GENERATE(ImageFileFormat::Png, ImageFileFormat::Tiff);
    const auto bitDepth = GENERATE(8, 16);
    const auto layout = GENERATE(PixelFormat::RgbU8, PixelFormat::RgbaU8, PixelFormat::RgbaU16,
                                 PixelFormat::RgbaF32);
    CAPTURE(format, bitDepth, layout);
    const test::TempDir directory;
    const auto destination = directory.file("rainbow.bin");
    const auto image = test::rainbow({5, 3}, layout);
    const auto reference = test::rainbow({5, 3}, PixelFormat::RgbaF32);
    const auto expected = reference.samples<float>();
    const auto original = std::vector(image.bytes().begin(), image.bytes().end());

    exportImage(image, destination, {.format = format, .bitDepth = bitDepth});

    const QImage decoded(QFile(destination).fileName());
    REQUIRE_FALSE(decoded.isNull());
    REQUIRE(decoded.width() == 5);
    REQUIRE(decoded.height() == 3);
    REQUIRE(decoded.pixelFormat().redSize() == bitDepth);
    REQUIRE(decoded.pixelFormat().greenSize() == bitDepth);
    REQUIRE(decoded.pixelFormat().blueSize() == bitDepth);
    REQUIRE(decoded.colorSpace() == QColorSpace(QColorSpace::SRgb));
    REQUIRE(std::ranges::equal(image.bytes(), original));
    for (int y = 0; y < decoded.height(); ++y) {
        for (int x = 0; x < decoded.width(); ++x) {
            const auto base = static_cast<std::size_t>((y * decoded.width() + x) * 4);
            const auto pixel = decoded.pixelColor(x, y);
            REQUIRE(std::abs(pixel.redF() - expected[base]) < 0.004F);
            REQUIRE(std::abs(pixel.greenF() - expected[base + 1]) < 0.004F);
            REQUIRE(std::abs(pixel.blueF() - expected[base + 2]) < 0.004F);
            REQUIRE(pixel.alphaF() == 1.0F);
        }
    }
}

TEST_CASE("16-bit exports preserve precision and straight alpha", "[ImageExport]") {
    const auto format = GENERATE(ImageFileFormat::Png, ImageFileFormat::Tiff);
    const test::TempDir directory;
    const auto destination = directory.file("precision.bin");
    ImageBuffer image({1, 1}, PixelFormat::RgbaU16, NamedEncoding::Srgb);
    const std::array<std::uint16_t, 4> expected = {0x1234, 0x5678, 0x9ABC, 0x4567};
    std::ranges::copy(expected, image.samples<std::uint16_t>().begin());

    exportImage(image, destination, {.format = format, .bitDepth = 16});

    const QImage decoded(QFile(destination).fileName());
    REQUIRE_FALSE(decoded.isNull());
    REQUIRE(decoded.pixelFormat().redSize() == 16);
    const auto pixel = decoded.pixelColor(0, 0).rgba64();
    REQUIRE(pixel.red() == expected[0]);
    REQUIRE(pixel.green() == expected[1]);
    REQUIRE(pixel.blue() == expected[2]);
    REQUIRE(pixel.alpha() == expected[3]);
}

TEST_CASE("Invalid export options preserve an existing destination", "[ImageExport]") {
    const auto options = GENERATE(ExportOptions{.bitDepth = 0}, ExportOptions{.bitDepth = 12},
                                  ExportOptions{.format = ImageFileFormat::Jpeg, .bitDepth = 16},
                                  ExportOptions{.format = ImageFileFormat::Jpeg, .quality = -1},
                                  ExportOptions{.format = ImageFileFormat::Jpeg, .quality = 101},
                                  ExportOptions{.encoding = workingEncoding});
    const test::TempDir directory;
    const auto destination = directory.file("existing.png");
    const auto image = test::rainbow();
    exportImage(image, destination, {});
    const auto original = head(destination, std::filesystem::file_size(destination));

    REQUIRE_THROWS_AS(exportImage(image, destination, options), std::invalid_argument);
    REQUIRE(std::filesystem::file_size(destination) == original.size());
    REQUIRE(head(destination, original.size()) == original);
}

TEST_CASE("Working-space input is converted to the output encoding", "[ImageExport]") {
    const test::TempDir directory;
    const auto destination = directory.file("linear.png");
    const auto image = test::rainbow({3, 2}, PixelFormat::RgbaF32, workingEncoding);

    exportImage(image, destination, {});

    REQUIRE(startsWith(destination, pngSignature));
}

TEST_CASE("The working encoding is refused as an output encoding", "[ImageExport]") {
    const test::TempDir directory;
    const auto destination = directory.file("linear.png");
    const auto image = test::rainbow({3, 2}, PixelFormat::RgbaU8);

    ExportOptions options;
    options.encoding = workingEncoding;

    REQUIRE_THROWS_AS(exportImage(image, destination, options), std::invalid_argument);
    REQUIRE_FALSE(std::filesystem::exists(destination));
}

TEST_CASE("A camera-native buffer is refused before the encoder", "[ImageExport]") {
    // Its primaries belong to one sensor, so no output profile could describe
    // them to a viewer. The buffer has to pass white balance first (ADR 007).
    const test::TempDir directory;
    const auto destination = directory.file("camera.png");
    ImageBuffer image({2, 2}, PixelFormat::RgbaU16, CameraNative{});

    REQUIRE_THROWS_AS(exportImage(image, destination, {}), std::invalid_argument);
    REQUIRE_FALSE(std::filesystem::exists(destination));
}

TEST_CASE("JPEG rejects transparency before quantisation", "[ImageExport]") {
    const auto layout = GENERATE(PixelFormat::RgbaU8, PixelFormat::RgbaU16, PixelFormat::RgbaF32);
    const test::TempDir directory;
    const auto destination = directory.file("transparent.jpg");
    auto image = test::rainbow({1, 1}, layout);
    if (layout == PixelFormat::RgbaU8) {
        image.samples<std::uint8_t>()[3] = 254;
    } else if (layout == PixelFormat::RgbaU16) {
        image.samples<std::uint16_t>()[3] = 65534;
    } else {
        image.samples<float>()[3] = std::nextafter(1.0F, 0.0F);
    }

    REQUIRE_THROWS_AS(exportImage(image, destination, {}), std::invalid_argument);
    REQUIRE_FALSE(std::filesystem::exists(destination));
}

TEST_CASE("Quality changes JPEG output and is ignored for lossless formats", "[ImageExport]") {
    const auto format =
        GENERATE(ImageFileFormat::Jpeg, ImageFileFormat::Png, ImageFileFormat::Tiff);
    const test::TempDir directory;
    const auto destination = directory.file("quality.bin");
    const auto image = test::rainbow({64, 32});
    exportImage(image, destination, {.format = format, .quality = 0});
    const auto lowQuality = head(destination, std::filesystem::file_size(destination));
    exportImage(image, destination, {.format = format, .quality = 100});
    const auto highQuality = head(destination, std::filesystem::file_size(destination));

    if (format == ImageFileFormat::Jpeg) {
        REQUIRE(lowQuality != highQuality);
        REQUIRE_FALSE(QImage(QFile(destination).fileName()).isNull());
    } else {
        REQUIRE(lowQuality == highQuality);
        REQUIRE_NOTHROW(exportImage(image, destination, {.format = format, .quality = -1}));
    }
}

TEST_CASE("Export converts sRGB primaries and optionally embeds the output profile",
          "[ImageExport]") {
    const auto format = GENERATE(ImageFileFormat::Png, ImageFileFormat::Tiff);
    const auto encoding =
        GENERATE(NamedEncoding::Srgb, NamedEncoding::DisplayP3, NamedEncoding::AdobeRgb);
    const test::TempDir directory;
    const auto destination = directory.file("colour.bin");
    ImageBuffer image({1, 1}, PixelFormat::RgbaF32, NamedEncoding::Srgb);
    const std::array<float, 4> red = {1.0F, 0.0F, 0.0F, 1.0F};
    std::ranges::copy(red, image.samples<float>().begin());

    exportImage(image, destination, {.format = format, .encoding = encoding, .bitDepth = 16});
    const QImage tagged(QFile(destination).fileName());
    REQUIRE_FALSE(tagged.isNull());

    /// Reference values for sRGB red transformed via XYZ into the target RGB space.
    std::array<float, 3> expected = {1.0F, 0.0F, 0.0F};
    QColorSpace target(QColorSpace::SRgb);
    if (encoding == NamedEncoding::DisplayP3) {
        expected = {0.91749F, 0.20029F, 0.13856F};
        target = QColorSpace(QColorSpace::DisplayP3);
    } else if (encoding == NamedEncoding::AdobeRgb) {
        expected = {0.8586F, 0.0F, 0.0F};
        target = QColorSpace(QColorSpace::AdobeRgb);
    }
    REQUIRE(tagged.colorSpace() == target);
    const auto pixel = tagged.pixelColor(0, 0);
    REQUIRE(std::abs(pixel.redF() - expected[0]) < 0.002F);
    REQUIRE(std::abs(pixel.greenF() - expected[1]) < 0.002F);
    REQUIRE(std::abs(pixel.blueF() - expected[2]) < 0.002F);

    exportImage(image, destination,
                {.format = format, .encoding = encoding, .bitDepth = 16, .embedProfile = false});
    const QImage untagged(QFile(destination).fileName());
    REQUIRE_FALSE(untagged.isNull());
    REQUIRE_FALSE(untagged.colorSpace().isValid());
    REQUIRE(untagged.pixelColor(0, 0) == pixel);
}

TEST_CASE("Export uses the source colour encoding", "[ImageExport]") {
    const auto encoding = GENERATE(NamedEncoding::DisplayP3, NamedEncoding::AdobeRgb);
    const test::TempDir directory;
    const auto destination = directory.file("source.png");
    ImageBuffer image({1, 1}, PixelFormat::RgbaF32, encoding);
    const bool isP3 = encoding == NamedEncoding::DisplayP3;
    const std::array<float, 4> samples =
        isP3 ? std::array<float, 4>{0.91749F, 0.20029F, 0.13856F, 1.0F}
             : std::array<float, 4>{0.5F, 0.5F, 0.5F, 1.0F};
    std::ranges::copy(samples, image.samples<float>().begin());

    exportImage(image, destination, {.bitDepth = 16});

    const QImage decoded(QFile(destination).fileName());
    REQUIRE_FALSE(decoded.isNull());
    const auto pixel = decoded.pixelColor(0, 0);
    /// Adobe RGB's gamma-encoded mid-grey expressed through the sRGB transfer function.
    const float grey = 1.055F * std::pow(std::pow(0.5F, 563.0F / 256.0F), 1.0F / 2.4F) - 0.055F;
    REQUIRE(std::abs(pixel.redF() - (isP3 ? 1.0F : grey)) < 0.002F);
    REQUIRE(std::abs(pixel.greenF() - (isP3 ? 0.0F : grey)) < 0.002F);
    REQUIRE(std::abs(pixel.blueF() - (isP3 ? 0.0F : grey)) < 0.002F);
}

TEST_CASE("JPEG embeds the requested profile only when enabled", "[ImageExport]") {
    const auto embedProfile = GENERATE(false, true);
    const test::TempDir directory;
    const auto destination = directory.file("profile.jpg");
    exportImage(test::rainbow(), destination,
                {.encoding = NamedEncoding::DisplayP3, .embedProfile = embedProfile});

    const QImage decoded(QFile(destination).fileName());
    REQUIRE_FALSE(decoded.isNull());
    REQUIRE(decoded.colorSpace().isValid() == embedProfile);
    if (embedProfile) {
        REQUIRE(decoded.colorSpace() == QColorSpace(QColorSpace::DisplayP3));
    }
}
