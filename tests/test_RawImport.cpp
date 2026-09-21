#include "ImageImport.h"

#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>

using namespace arraw;

/// Integration tests over RAW import. Black box, like test_RoundTrip.cpp: they
/// go through loadImage rather than the LibRaw wrapper, because which decoder
/// a file reaches is itself part of what needs testing. The fixtures are
/// synthetic DNGs from tests/fixtures/make_raw_fixtures.py; see ADR 005.

namespace {

constexpr std::string_view neutralFixture = "linear-32x24-neutral.dng";
constexpr std::string_view warmFixture = "linear-32x24-warmwb.dng";
constexpr std::string_view rotatedFixture = "linear-32x24-rotated.dng";
constexpr std::string_view bayerFixture = "bayer-32x24.dng";

/// @brief Reads one pixel's four samples from a 16-bit RGBA buffer.
std::span<const std::uint16_t> pixelAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto samples = image.samples<std::uint16_t>();
    const auto base = (static_cast<std::size_t>(y) * image.size().width + x) * 4;
    return samples.subspan(base, 4);
}

/// @brief The sample value the generator's ramp holds at a column.
std::uint16_t rampAt(std::uint32_t x, std::uint32_t width) {
    return static_cast<std::uint16_t>(std::lround(static_cast<double>(x) / (width - 1) * 65535.0));
}

/// @brief Largest absolute difference between two buffers of equal shape.
int maxDifference(const ImageBuffer& left, const ImageBuffer& right) {
    const auto a = left.samples<std::uint16_t>();
    const auto b = right.samples<std::uint16_t>();
    REQUIRE(a.size() == b.size());

    int worst = 0;
    for (std::size_t index = 0; index < a.size(); ++index) {
        worst = std::max(worst, std::abs(static_cast<int>(a[index]) - static_cast<int>(b[index])));
    }
    return worst;
}

/// @brief Copies a fixture into a temporary directory under a different name.
std::filesystem::path copyAs(const test::TempDir& directory, std::string_view fixture,
                             std::string_view name) {
    const auto destination = directory.file(name);
    std::filesystem::copy_file(test::fixture(fixture), destination);
    return destination;
}

} // namespace

TEST_CASE("A RAW arrives in the working encoding as opaque 16-bit RGBA", "[integration][raw]") {
    const auto image = loadImage(test::fixture(neutralFixture));

    REQUIRE(image.size() == ImageSize{32, 24});
    REQUIRE(image.format() == PixelFormat::RgbaU16);
    REQUIRE(image.encoding() == workingEncoding);

    /// LibRaw emits three channels; the fourth is synthesised so the buffer is
    /// in a layout exportImage can write back.
    const auto samples = image.samples<std::uint16_t>();
    for (std::size_t index = 3; index < samples.size(); index += 4) {
        REQUIRE(samples[index] == 65535);
    }
}

TEST_CASE("A linear DNG decodes to the values it stores", "[integration][raw]") {
    const auto image = loadImage(test::fixture(neutralFixture));
    const auto width = image.size().width;

    /// The sharpest assertion in the suite, and the reason this fixture exists.
    /// The file is a neutral linear ramp with a unity as-shot neutral and an
    /// sRGB camera matrix, so with no_auto_bright set and an output gamma of
    /// 1.0 the decoder has nothing to do but a colour-space rotation. Measured
    /// max error: 1 code in 65535, mean 0.33. An automatic brightness stretch
    /// would miss by thousands of codes, and a stray transfer function by tens
    /// of thousands -- neither can hide behind this bound.
    int worst = 0;
    int worstSpread = 0;
    for (std::uint32_t y = 0; y < image.size().height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto pixel = pixelAt(image, x, y);
            const int expected = rampAt(x, width);
            for (int channel = 0; channel < 3; ++channel) {
                worst = std::max(worst, std::abs(static_cast<int>(pixel[channel]) - expected));
            }
            // A neutral input must stay neutral: the camera matrix is declared
            // as sRGB, so any channel spread is a colour cast the decode invented.
            worstSpread = std::max({worstSpread, std::abs(static_cast<int>(pixel[0]) - pixel[1]),
                                    std::abs(static_cast<int>(pixel[1]) - pixel[2])});
        }
    }
    CAPTURE(worst, worstSpread);
    REQUIRE(worst <= 1);
    REQUIRE(worstSpread <= 1);
}

TEST_CASE("The camera's as-shot white balance is applied", "[integration][raw]") {
    const auto neutral = loadImage(test::fixture(neutralFixture));
    const auto warm = loadImage(test::fixture(warmFixture));

    /// Same pixels, same everything, except an AsShotNeutral of (0.5, 1.0, 0.8)
    /// -- multipliers of (2.0, 1.0, 1.25). The ordering is what is asserted
    /// rather than the magnitudes, because LibRaw normalises the multipliers
    /// before the colour matrix mixes them.
    const auto midpoint = pixelAt(warm, warm.size().width / 2, warm.size().height / 2);
    REQUIRE(midpoint[0] > midpoint[2]); // red lifted most
    REQUIRE(midpoint[2] > midpoint[1]); // blue lifted more than green

    /// And it is genuinely a different decode, not a rounding difference.
    REQUIRE(maxDifference(neutral, warm) > 1000);
}

TEST_CASE("A RAW's orientation tag is not baked into the buffer", "[integration][raw]") {
    const auto neutral = loadImage(test::fixture(neutralFixture));
    const auto rotated = loadImage(test::fixture(rotatedFixture));

    /// Orientation 6 is "rotate 90 clockwise". LibRaw would honour it by
    /// default (user_flip defaults to -1), which would return 24x32. arraw
    /// keeps rotation a develop setting, so the frame must stay as stored --
    /// the same promise loadImage already makes for JPEG and TIFF.
    REQUIRE(rotated.size() == ImageSize{32, 24});
    REQUIRE(maxDifference(neutral, rotated) == 0);
}

TEST_CASE("A mosaic is demosaiced", "[integration][raw]") {
    const auto image = loadImage(test::fixture(bayerFixture));

    REQUIRE(image.size() == ImageSize{32, 24});

    /// The fixture is a flat RGGB field holding three different constants, so
    /// undemosaiced it is a checkerboard and demosaiced it is one flat colour.
    /// Uniformity therefore proves interpolation ran, and the channel ordering
    /// proves the CFA pattern was read rather than assumed.
    const auto reference = pixelAt(image, 0, 0);
    for (std::uint32_t y = 0; y < image.size().height; ++y) {
        for (std::uint32_t x = 0; x < image.size().width; ++x) {
            const auto pixel = pixelAt(image, x, y);
            CAPTURE(x, y);
            REQUIRE(pixel[0] == reference[0]);
            REQUIRE(pixel[1] == reference[1]);
            REQUIRE(pixel[2] == reference[2]);
        }
    }
    REQUIRE(reference[0] > reference[1]);
    REQUIRE(reference[1] > reference[2]);
}

TEST_CASE("A misnamed RAW still loads, through content", "[integration][raw]") {
    const test::TempDir directory;
    const auto misnamed = copyAs(directory, rotatedFixture, "holiday.png");

    const auto image = loadImage(misnamed);

    /// The extension promised PNG and Qt's codecs declined -- a DNG is a TIFF
    /// by signature, so Qt reaches for its TIFF reader and that reader fails on
    /// the RAW tags. Content then has the last word through the LibRaw
    /// fallback, and the file loads.
    REQUIRE(image.size() == ImageSize{32, 24});
    REQUIRE(maxDifference(image, loadImage(test::fixture(neutralFixture))) == 0);
}

TEST_CASE("A RAW extension arraw does not list is still decoded by arraw", "[integration][raw]") {
    const test::TempDir directory;
    const auto foreign = copyAs(directory, rotatedFixture, "holiday.mrw");

    const auto image = loadImage(foreign);

    /// The case that makes the "raw"-format decline in ImageImport.cpp load
    /// bearing, and the only one that can observe *which* decoder answered.
    ///
    /// KDE's kimg_raw plugin claims files by extension, and it claims eleven
    /// RAW extensions beyond the ten arraw routes to LibRaw itself -- .mrw,
    /// .srf, .x3f, .kdc, .mos, .raw, .3fr, .iiq, .erf, .nrw, .crw. Left to it,
    /// this file decodes 24x32, because it honours the orientation tag. arraw
    /// must return 32x24.
    ///
    /// It is also the divergence that guard exists to prevent: on a machine
    /// without kf6-kimageformats the same file is detected as TIFF, fails, and
    /// reaches LibRaw through the fallback. Both machines must agree, so this
    /// test is meaningful whether or not the plugin is installed.
    REQUIRE(image.size() == ImageSize{32, 24});
    REQUIRE(maxDifference(image, loadImage(test::fixture(neutralFixture))) == 0);
}

TEST_CASE("A file that is neither an image nor a RAW is refused", "[integration][raw]") {
    const test::TempDir directory;
    const auto destination = directory.file("notes.png");
    {
        std::ofstream stream(destination, std::ios::binary);
        stream << "not an image at all";
        REQUIRE(stream.good());
    }

    REQUIRE_THROWS_AS(loadImage(destination), std::runtime_error);
}
