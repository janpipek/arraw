#include "ImageExport.h"
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
#include <variant>

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
constexpr std::string_view highMaxFixture = "linear-32x24-highmax.dng";
constexpr std::string_view noWbFixture = "linear-32x24-nowb.dng";
constexpr std::string_view noWbDarkFixture = "linear-32x24-nowb-dark.dng";
constexpr std::string_view skewedFixture = "linear-32x24-skewed.dng";
constexpr std::string_view previewFixture = "preview-32x24.dng";
constexpr std::string_view testCard = "testcard-61x41-srgb8.png";

/// @brief Reads one pixel's four samples from a 16-bit RGBA buffer.
std::span<const std::uint16_t> pixelAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto samples = image.samples<std::uint16_t>();
    const auto base = (static_cast<std::size_t>(y) * image.size().width + x) * 4;
    return samples.subspan(base, 4);
}

/// @brief Returns the sample value the generator's ramp holds at a column.
std::uint16_t rampAt(std::uint32_t x, std::uint32_t width) {
    return static_cast<std::uint16_t>(std::lround(static_cast<double>(x) / (width - 1) * 65535.0));
}

/// @brief Returns the largest absolute difference between two buffers of equal shape.
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

TEST_CASE("A RAW arrives in its camera's own encoding as opaque 16-bit RGBA",
          "[integration][raw]") {
    const auto image = loadImage(test::fixture(neutralFixture));

    REQUIRE(image.size() == ImageSize{32, 24});
    REQUIRE(image.format() == PixelFormat::RgbaU16);

    /// Not the working encoding: a white balance is a per-channel gain in the
    /// space the sensor recorded, so the conversion out of it is development's
    /// job rather than the decoder's (ADR 007).
    REQUIRE_FALSE(isWorkingEncoding(image.encoding()));
    const auto* camera = std::get_if<CameraNative>(&image.encoding());
    REQUIRE(camera != nullptr);

    /// Whatever the primaries, the transform out of them must leave white
    /// alone, which is the statement that each row sums to one.
    for (std::size_t row = 0; row < 3; ++row) {
        const float sum = camera->toWorking.at(row, 0) + camera->toWorking.at(row, 1) +
                          camera->toWorking.at(row, 2);
        REQUIRE(std::abs(sum - 1.0F) < 1e-4F);
    }

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

TEST_CASE("A RAW carries the gains its camera recorded", "[integration][raw]") {
    const auto warm = loadImage(test::fixture(warmFixture));
    const auto* camera = std::get_if<CameraNative>(&warm.encoding());
    REQUIRE(camera != nullptr);

    /// AsShotNeutral (0.5, 1.0, 0.8) is DNG's convention of neutral channel
    /// values; arraw stores their reciprocals, normalised so green is 1.
    REQUIRE(std::abs(camera->asShotMultipliers[0] - 2.0F) < 1e-3F);
    REQUIRE(std::abs(camera->asShotMultipliers[1] - 1.0F) < 1e-3F);
    REQUIRE(std::abs(camera->asShotMultipliers[2] - 1.25F) < 1e-3F);

    /// The decode used exactly them, so nothing later has to guess whether the
    /// buffer is balanced.
    REQUIRE(camera->appliedMultipliers == camera->asShotMultipliers);
}

TEST_CASE("A RAW without an as-shot neutral says so, and what was used instead",
          "[integration][raw]") {
    const auto image = loadImage(test::fixture(noWbFixture));
    const auto* camera = std::get_if<CameraNative>(&image.encoding());
    REQUIRE(camera != nullptr);

    /// The file declares no white balance, so LibRaw is given the daylight
    /// multipliers its colour matrix implies instead (ADR 005). Recording both
    /// is what lets a temperature be reported honestly rather than invented.
    REQUIRE(camera->asShotMultipliers == Gains{1.0F, 1.0F, 1.0F});
    REQUIRE(camera->appliedMultipliers[1] == 1.0F);
}

TEST_CASE("A RAW's daylight calibration survives the decode", "[integration][raw]") {
    const auto image = loadImage(test::fixture(neutralFixture));
    const auto* camera = std::get_if<CameraNative>(&image.encoding());
    REQUIRE(camera != nullptr);

    /// LibRaw normalises the camera matrix's rows before inverting it and
    /// keeps the divisors in pre_mul, then overwrites pre_mul during
    /// processing. Reading it too late loses the calibration entirely, and it
    /// cannot be recovered from the matrix, which is invariant to it.
    for (const float scale : camera->daylightScale) {
        REQUIRE(scale > 0.0F);
    }
}

TEST_CASE("A camera that is not sRGB keeps both halves of its calibration", "[integration][raw]") {
    const auto image = loadImage(test::fixture(skewedFixture));
    const auto* camera = std::get_if<CameraNative>(&image.encoding());
    REQUIRE(camera != nullptr);

    /// The fixture's matrix has scaled rows, which LibRaw divides out and
    /// keeps in pre_mul. Every other fixture here has a unity calibration, so
    /// this is the only one that can tell whether it was stored at all.
    REQUIRE(camera->daylightScale[0] < 0.6F);
    REQUIRE(camera->daylightScale[2] > 1.2F);

    /// And its red row is mixed with green, so the transform out of the
    /// sensor's primaries is a real one rather than the identity.
    REQUIRE_FALSE(camera->toWorking == Matrix3::identity());

    /// It still has to leave white alone, mixed rows or not.
    for (std::size_t row = 0; row < 3; ++row) {
        const float sum = camera->toWorking.at(row, 0) + camera->toWorking.at(row, 1) +
                          camera->toWorking.at(row, 2);
        REQUIRE(std::abs(sum - 1.0F) < 1e-4F);
    }
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

TEST_CASE("A RAW that carries a preview decodes the photograph, not the preview",
          "[integration][raw]") {
    const auto image = loadImage(test::fixture(previewFixture));

    /// The fixture has the shape of a real camera file: an ordinary 8x6 RGB
    /// preview in IFD0, the sensor data in a sub-IFD. Its sub-IFD holds the
    /// same ramp as the neutral fixture, so the photograph is recognisable
    /// pixel for pixel -- and the preview, flat magenta, is unmistakably not.
    REQUIRE(image.size() == ImageSize{32, 24});
    REQUIRE(maxDifference(image, loadImage(test::fixture(neutralFixture))) == 0);
}

TEST_CASE("A preview-carrying RAW decodes the same under any name", "[integration][raw]") {
    const test::TempDir directory;

    /// A RAW container is a TIFF, and a TIFF reader decodes its preview
    /// happily -- so a decoder chosen by extension alone hands back an 8x6
    /// thumbnail for a file it does not recognise by name. Which is every
    /// renamed file, and every RAW extension arraw does not list.
    for (const auto* name : {"holiday.png", "holiday.mrw", "holiday.tif"}) {
        CAPTURE(name);
        const auto image = loadImage(copyAs(directory, previewFixture, name));
        REQUIRE(image.size() == ImageSize{32, 24});
        REQUIRE(maxDifference(image, loadImage(test::fixture(neutralFixture))) == 0);
    }
}

TEST_CASE("One frame's brightest pixel does not change how it is exposed", "[integration][raw]") {
    const auto image = loadImage(test::fixture(highMaxFixture));

    /// Two flat neutral halves, 16000 and 52000, under a declared white level
    /// of 65535. Both must arrive as they are stored. LibRaw would otherwise
    /// lower the white level to the frame's own maximum whenever that maximum
    /// sits above adjust_maximum_thr (0.75) of the declared one, stretching
    /// 16000 to 20164 here -- the per-frame brightness dependence that
    /// no_auto_bright is only half of.
    const auto dark = pixelAt(image, 0, 0);
    const auto bright = pixelAt(image, image.size().width - 1, 0);
    CAPTURE(dark[0], bright[0]);
    for (int channel = 0; channel < 3; ++channel) {
        REQUIRE(std::abs(static_cast<int>(dark[channel]) - 16000) <= 1);
        REQUIRE(std::abs(static_cast<int>(bright[channel]) - 52000) <= 1);
    }
}

TEST_CASE("A RAW without a camera white balance keeps its colours", "[integration][raw]") {
    const auto image = loadImage(test::fixture(noWbFixture));
    const auto other = loadImage(test::fixture(noWbDarkFixture));

    /// Two fixtures, both a flat (48000, 32000, 16000) field with no
    /// AsShotNeutral tag, differing only in a right half the second one
    /// darkens. arraw answers a missing as-shot neutral with the daylight
    /// multipliers the camera's colour matrix implies, which the frame cannot
    /// influence: the left half must decode the same in both. LibRaw's own
    /// answer is a white balance computed from the frame, which fails both
    /// assertions at once -- it differs between the two files, and it flattens
    /// this colour to (47999, 48000, 48000), a neutral grey the sensor never
    /// saw. Measured daylight result: (41346, 32922, 17934).
    const auto pixel = pixelAt(image, 4, image.size().height / 2);
    const auto same = pixelAt(other, 4, other.size().height / 2);
    CAPTURE(pixel[0], pixel[1], pixel[2], same[0], same[1], same[2]);
    for (int channel = 0; channel < 3; ++channel) {
        REQUIRE(std::abs(static_cast<int>(pixel[channel]) - same[channel]) <= 1);
    }
    REQUIRE(pixel[0] > pixel[1]);
    REQUIRE(pixel[1] > pixel[2]);
    REQUIRE(pixel[0] - pixel[2] > 10000);
}

TEST_CASE("An exported TIFF is read as an image, not as sensor data", "[integration][raw]") {
    const test::TempDir directory;
    const auto destination = directory.file("export.tif");
    const auto original = loadImage(test::fixture(testCard));
    exportImage(original, destination, {.format = ImageFileFormat::Tiff, .bitDepth = 16});

    const auto reloaded = loadImage(destination);

    /// Content is consulted before Qt, so this pins what "content" may claim:
    /// LibRaw opens camera files, not ordinary TIFFs, and arraw writes
    /// ordinary TIFFs. Measured with LibRaw 0.22.2, which declines every
    /// multi-channel TIFF offered to it. If a later LibRaw grows greedier,
    /// this fails before a user notices their own exports decoding through
    /// the wrong path.
    REQUIRE(reloaded.size() == original.size());
    REQUIRE(reloaded.format() == original.format());
}
