#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <Diagnostics.h>
#include <ImageImport.h>

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <variant>
#include <vector>

using namespace arraw;

/// Integration tests over embedded previews. Black box through
/// readEmbeddedPreview; the JPEG fixtures are written here with exiv2, so a
/// test can say which way up the preview is stored and which way the file says
/// it should be shown. See ADR 031.

namespace {

namespace fs = std::filesystem;

constexpr std::uint8_t reddish = 220;

/// @brief Encodes an image as JPEG, as a camera embeds it.
QByteArray jpegBytes(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    REQUIRE(image.save(&buffer, "JPEG", 100));
    return bytes;
}

/// @brief Builds an image whose left half is red and whose right half is blue.
QImage leftRedRightBlue(int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y, x < width / 2 ? QColor(255, 0, 0) : QColor(0, 0, 255));
        }
    }
    return image;
}

/// @brief Writes a JPEG of a given size whose EXIF thumbnail is another image.
/// @param orientation The file's Exif.Image.Orientation, or zero for none.
fs::path jpegWithThumbnail(const test::TempDir& directory, const QImage& thumbnail,
                           int orientation) {
    const auto path = directory.file("shot.jpg");
    QImage main(96, 64, QImage::Format_RGB32);
    main.fill(Qt::gray);
    REQUIRE(main.save(QString::fromStdString(path.string()), "JPEG"));
    auto file = Exiv2::ImageFactory::open(path.string());
    file->readMetadata();
    if (orientation != 0) {
        file->exifData()["Exif.Image.Orientation"] = static_cast<std::uint16_t>(orientation);
    }
    const QByteArray bytes = jpegBytes(thumbnail);
    Exiv2::ExifThumb(file->exifData())
        .setJpegThumbnail(reinterpret_cast<const Exiv2::byte*>(bytes.constData()),
                          static_cast<size_t>(bytes.size()));
    file->writeMetadata();
    return path;
}

/// @brief Reads one pixel of an RGBA8 buffer.
std::array<std::uint8_t, 3> pixelAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto samples = image.samples<std::uint8_t>();
    const auto base = (static_cast<std::size_t>(y) * image.size().width + x) * 4;
    return {samples[base], samples[base + 1], samples[base + 2]};
}

bool isRed(const std::array<std::uint8_t, 3>& pixel) {
    return pixel[0] > reddish && pixel[2] < 40;
}

bool isBlue(const std::array<std::uint8_t, 3>& pixel) {
    return pixel[2] > reddish && pixel[0] < 40;
}

} // namespace

TEST_CASE("An EXIF thumbnail is returned as an sRGB 8-bit buffer", "[preview][integration]") {
    const test::TempDir directory;
    const auto path = jpegWithThumbnail(directory, leftRedRightBlue(32, 16), 0);
    CollectedDiagnostics log;
    const auto preview = readEmbeddedPreview(path, 0, log);
    REQUIRE(preview);
    REQUIRE(preview->size() == ImageSize{32, 16});
    REQUIRE(preview->format() == PixelFormat::RgbaU8);
    REQUIRE(std::get<NamedEncoding>(preview->encoding()) == NamedEncoding::Srgb);
    REQUIRE(preview->orientation() == ImageOrientation::Normal);
    REQUIRE(isRed(pixelAt(*preview, 2, 8)));
    REQUIRE(isBlue(pixelAt(*preview, 29, 8)));
    REQUIRE(pixelAt(*preview, 2, 8)[0] > 0);
    REQUIRE(log.entries().empty());
}

TEST_CASE("A preview is turned upright by the orientation the file records",
          "[preview][integration]") {
    const test::TempDir directory;
    const QImage thumbnail = leftRedRightBlue(32, 16);

    SECTION("rotated a quarter turn, which swaps its sides") {
        const auto preview = readEmbeddedPreview(jpegWithThumbnail(directory, thumbnail, 6), 0);
        REQUIRE(preview);
        REQUIRE(preview->size() == ImageSize{16, 32});
        // Turned clockwise, the left half of the stored image is the top.
        REQUIRE(isRed(pixelAt(*preview, 8, 2)));
        REQUIRE(isBlue(pixelAt(*preview, 8, 29)));
    }
    SECTION("rotated the other way") {
        const auto preview = readEmbeddedPreview(jpegWithThumbnail(directory, thumbnail, 8), 0);
        REQUIRE(preview);
        REQUIRE(preview->size() == ImageSize{16, 32});
        REQUIRE(isBlue(pixelAt(*preview, 8, 2)));
        REQUIRE(isRed(pixelAt(*preview, 8, 29)));
    }
    SECTION("upside down") {
        const auto preview = readEmbeddedPreview(jpegWithThumbnail(directory, thumbnail, 3), 0);
        REQUIRE(preview);
        REQUIRE(preview->size() == ImageSize{32, 16});
        REQUIRE(isBlue(pixelAt(*preview, 2, 8)));
        REQUIRE(isRed(pixelAt(*preview, 29, 8)));
    }
    SECTION("mirrored") {
        const auto preview = readEmbeddedPreview(jpegWithThumbnail(directory, thumbnail, 2), 0);
        REQUIRE(preview);
        REQUIRE(isBlue(pixelAt(*preview, 2, 8)));
        REQUIRE(isRed(pixelAt(*preview, 29, 8)));
    }
    SECTION("transposed, then transversed") {
        // 5 mirrors about the main diagonal, 7 about the other one.
        const auto transposed = readEmbeddedPreview(jpegWithThumbnail(directory, thumbnail, 5), 0);
        REQUIRE(transposed);
        REQUIRE(transposed->size() == ImageSize{16, 32});
        REQUIRE(isRed(pixelAt(*transposed, 8, 2)));
        const auto transversed = readEmbeddedPreview(jpegWithThumbnail(directory, thumbnail, 7), 0);
        REQUIRE(transversed);
        REQUIRE(isBlue(pixelAt(*transversed, 8, 2)));
    }
}

TEST_CASE("A preview is reduced to fit, never enlarged", "[preview][integration]") {
    const test::TempDir directory;
    const auto path = jpegWithThumbnail(directory, leftRedRightBlue(64, 32), 6);

    SECTION("by its longer edge, whatever way up it ends") {
        const auto preview = readEmbeddedPreview(path, 16);
        REQUIRE(preview);
        REQUIRE(preview->size() == ImageSize{8, 16});
    }
    SECTION("not at all when it already fits") {
        const auto preview = readEmbeddedPreview(path, 512);
        REQUIRE(preview);
        REQUIRE(preview->size() == ImageSize{32, 64});
    }
}

TEST_CASE("A RAW's lone preview is used whatever size is asked for", "[preview][integration]") {
    // preview-32x24.dng carries one 8x6 preview, which is the largest and the
    // smallest alike; it is reduced to fit, never enlarged. Choosing among
    // several previews has no fixture, as the synthetic ones carry at most one.
    for (const std::uint32_t edge : {0u, 4u, 8u, 1000u}) {
        const auto preview = readEmbeddedPreview(test::fixture("preview-32x24.dng"), edge);
        REQUIRE(preview);
        REQUIRE(preview->size().width == std::min(8u, edge == 0 ? 8u : edge));
    }
}

TEST_CASE("A file without a preview gives nothing and says nothing", "[preview][integration]") {
    CollectedDiagnostics log;
    REQUIRE_FALSE(readEmbeddedPreview(test::fixture("testcard-61x41-srgb8.png"), 256, log));
    REQUIRE(log.entries().empty());
}

TEST_CASE("A file that cannot be read gives nothing and a warning", "[preview][integration]") {
    const test::TempDir directory;
    SECTION("missing") {
        CollectedDiagnostics log;
        REQUIRE_FALSE(readEmbeddedPreview(directory.file("absent.jpg"), 256, log));
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::PreviewUnreadable);
        REQUIRE(log.entries().front().severity == Severity::Warning);
    }
    SECTION("not an image") {
        std::ofstream(directory.file("notes.txt")) << "not a photograph";
        CollectedDiagnostics log;
        REQUIRE_FALSE(readEmbeddedPreview(directory.file("notes.txt"), 256, log));
        REQUIRE(log.entries().size() == 1);
        REQUIRE(log.entries().front().notice == Notice::PreviewUnreadable);
    }
}

TEST_CASE("Previews can be read from several threads at once", "[preview][integration]") {
    const test::TempDir directory;
    const auto path = jpegWithThumbnail(directory, leftRedRightBlue(32, 16), 6);
    std::atomic<int> correct = 0;
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&] {
            for (int round = 0; round < 10; ++round) {
                const auto preview = readEmbeddedPreview(path, 0);
                if (preview && preview->size() == ImageSize{16, 32} &&
                    isRed(pixelAt(*preview, 8, 2))) {
                    ++correct;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    REQUIRE(correct == 80);
}
