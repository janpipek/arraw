#include "Develop.h"
#include "ImageImport.h"
#include "WhiteBalance.h"

#include "support/Fixtures.h"
#include "support/TestImages.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>

using namespace arraw;

namespace {

constexpr std::string_view neutralFixture = "linear-32x24-neutral.dng";
constexpr std::string_view skewedFixture = "linear-32x24-skewed.dng";
constexpr std::string_view skewedNoWbFixture = "linear-32x24-skewed-nowb.dng";
constexpr std::string_view testCard = "testcard-61x41-srgb8.png";

/// @brief Reads one pixel's four samples from a developed buffer.
std::span<const float> pixelAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto samples = image.samples<float>();
    const auto base = (static_cast<std::size_t>(y) * image.size().width + x) * 4;
    return samples.subspan(base, 4);
}

} // namespace

TEST_CASE("Development ends in the working encoding, whatever it started in", "[develop]") {
    const auto raw = develop(loadImage(test::fixture(neutralFixture)), {});
    const auto card = develop(loadImage(test::fixture(testCard)), {});

    REQUIRE(isWorkingEncoding(raw.encoding()));
    REQUIRE(isWorkingEncoding(card.encoding()));
    REQUIRE(raw.format() == workingFormat);
    REQUIRE(card.format() == workingFormat);
    REQUIRE(raw.size() == ImageSize{32, 24});
}

TEST_CASE("Default settings convert a RAW and do nothing else", "[develop]") {
    /// The flat, faithful rendering the command line's help text promises. The
    /// fixture's camera matrix is sRGB and its as-shot neutral is unity, so a
    /// stored ramp must arrive as the same ramp in unit float.
    const auto image = develop(loadImage(test::fixture(neutralFixture)), {});
    const auto width = image.size().width;

    float worst = 0.0F;
    for (std::uint32_t x = 0; x < width; ++x) {
        const auto pixel = pixelAt(image, x, 0);
        const float expected = static_cast<float>(x) / static_cast<float>(width - 1);
        for (int channel = 0; channel < 3; ++channel) {
            worst = std::max(worst, std::abs(pixel[channel] - expected));
        }
        REQUIRE(pixel[3] == 1.0F);
    }
    CAPTURE(worst);
    REQUIRE(worst < 0.002F);
}

TEST_CASE("Exposure is a doubling per stop", "[develop]") {
    const auto source = test::rainbow({4, 2}, PixelFormat::RgbaU16, workingEncoding);

    const auto flat = develop(source, {});
    const auto lifted = develop(source, {.exposure = 1.0F});
    const auto dropped = develop(source, {.exposure = -1.0F});

    for (std::size_t index = 0; index < flat.samples<float>().size(); index += 4) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float base = flat.samples<float>()[index + channel];
            REQUIRE(std::abs(lifted.samples<float>()[index + channel] - base * 2.0F) < 1e-5F);
            REQUIRE(std::abs(dropped.samples<float>()[index + channel] - base * 0.5F) < 1e-5F);
        }
    }
}

TEST_CASE("Exposure leaves alpha alone", "[develop]") {
    auto source = test::rainbow({2, 1}, PixelFormat::RgbaU16, workingEncoding);
    source.samples<std::uint16_t>()[3] = 32768;

    const auto developed = develop(source, {.exposure = 2.0F});

    /// No develop setting produces transparency, and a source that carried
    /// some keeps exactly what it had: four stops must not make it opaque.
    REQUIRE(std::abs(pixelAt(developed, 0, 0)[3] - 0.5F) < 1e-4F);
}

TEST_CASE("A working-encoded photograph passes through unchanged", "[develop]") {
    const auto source = test::rainbow({3, 2}, PixelFormat::RgbaF32, workingEncoding);

    const auto developed = develop(source, {});

    const auto before = source.samples<float>();
    const auto after = developed.samples<float>();
    REQUIRE(before.size() == after.size());
    for (std::size_t index = 0; index < before.size(); ++index) {
        REQUIRE(std::abs(after[index] - before[index]) < 1e-6F);
    }
}

TEST_CASE("Development refuses an encoding it cannot start from", "[develop]") {
    /// Import converts everything it decodes into the working encoding, so an
    /// sRGB buffer arriving here means a photograph skipped that step.
    const auto source = test::rainbow({2, 2}, PixelFormat::RgbaU8, NamedEncoding::Srgb);

    REQUIRE_THROWS_AS(develop(source, {}), std::invalid_argument);
}

TEST_CASE("A requested size is refused while it is unimplemented", "[develop]") {
    const auto source = test::rainbow({4, 4}, PixelFormat::RgbaU16, workingEncoding);

    REQUIRE_THROWS_AS(develop(source, {}, {.targetSize = ImageSize{2, 2}}), std::invalid_argument);
}

TEST_CASE("Asking for the light the camera saw changes nothing", "[develop]") {
    /// As Shot and a custom setting at the camera's own reading are the same
    /// photograph: the gains a temperature wants are divided by the gains the
    /// decode already applied, and those are the same numbers.
    const auto source = loadImage(test::fixture(skewedFixture));
    const auto* camera = std::get_if<CameraNative>(&source.encoding());
    REQUIRE(camera != nullptr);
    const auto asShot = asShotTemperature(*camera);

    const auto left = develop(source, {});
    const auto right = develop(source, {.whiteBalance = WhiteBalanceMode::Custom,
                                        .temperature = asShot.kelvin,
                                        .tint = asShot.tint});

    const auto before = left.samples<float>();
    const auto after = right.samples<float>();
    for (std::size_t index = 0; index < before.size(); ++index) {
        REQUIRE(std::abs(after[index] - before[index]) < 1e-4F);
    }
}

TEST_CASE("A lower temperature cools the developed photograph", "[develop]") {
    const auto source = loadImage(test::fixture(skewedFixture));

    const auto warm = develop(
        source, {.whiteBalance = WhiteBalanceMode::Custom, .temperature = 3000.0F, .tint = 0.0F});
    const auto cool = develop(
        source, {.whiteBalance = WhiteBalanceMode::Custom, .temperature = 9000.0F, .tint = 0.0F});

    /// Naming a warm light tells arraw to take red back out of the picture.
    const auto warmPixel = pixelAt(warm, 20, 12);
    const auto coolPixel = pixelAt(cool, 20, 12);
    REQUIRE(warmPixel[0] < coolPixel[0]);
    REQUIRE(warmPixel[2] > coolPixel[2]);
}

TEST_CASE("Tint moves without disturbing the temperature", "[develop]") {
    const auto source = loadImage(test::fixture(skewedFixture));

    const auto neutral = develop(
        source, {.whiteBalance = WhiteBalanceMode::Custom, .temperature = 5500.0F, .tint = 0.0F});
    const auto magenta = develop(
        source, {.whiteBalance = WhiteBalanceMode::Custom, .temperature = 5500.0F, .tint = 40.0F});

    /// A magenta shift is red and blue rising against green. Green is pinned
    /// at 1 in the *sensor's* channels, but the working space's green is a
    /// mixture of all three of them, so it moves a little too -- which is why
    /// the assertion is about the ratios rather than about green standing
    /// still.
    const auto before = pixelAt(neutral, 20, 12);
    const auto after = pixelAt(magenta, 20, 12);
    REQUIRE(after[0] / after[1] > before[0] / before[1]);
    REQUIRE(after[2] / after[1] > before[2] / before[1]);
}

TEST_CASE("An unset temperature or tint keeps the camera's own", "[develop]") {
    /// Two sliders, not one control: moving the temperature must not silently
    /// reset the tint, so an absent value means "leave that one alone".
    const auto source = loadImage(test::fixture(skewedFixture));
    const auto* camera = std::get_if<CameraNative>(&source.encoding());
    REQUIRE(camera != nullptr);
    const auto asShot = asShotTemperature(*camera);

    const auto partial =
        develop(source, {.whiteBalance = WhiteBalanceMode::Custom, .temperature = 6000.0F});
    const auto spelled = develop(
        source,
        {.whiteBalance = WhiteBalanceMode::Custom, .temperature = 6000.0F, .tint = asShot.tint});

    const auto left = partial.samples<float>();
    const auto right = spelled.samples<float>();
    for (std::size_t index = 0; index < left.size(); ++index) {
        REQUIRE(std::abs(left[index] - right[index]) < 1e-5F);
    }
}

TEST_CASE("A temperature is refused on a photograph with no sensor", "[develop]") {
    /// Kelvin is measured against a sensor's response. A JPEG has none, and
    /// gets the incremental setting instead, which is not implemented yet.
    const auto source = test::rainbow({2, 2}, PixelFormat::RgbaU16, workingEncoding);

    REQUIRE_THROWS_AS(
        develop(source, {.whiteBalance = WhiteBalanceMode::Custom, .temperature = 4000.0F}),
        std::invalid_argument);
}

TEST_CASE("A file that recorded no white balance still develops from what it got", "[develop]") {
    /// The camera recorded nothing, so the decode substituted the daylight
    /// balance its matrix implies. What the photograph *is* balanced for is
    /// that substitute, and an unnamed setting has to be resolved from it --
    /// resolving from the recorded placeholder would move the colour the
    /// moment a photographer touched the tint.
    const auto source = loadImage(test::fixture(skewedNoWbFixture));
    const auto* camera = std::get_if<CameraNative>(&source.encoding());
    REQUIRE(camera != nullptr);

    /// The two readings genuinely differ on this camera, which is what makes
    /// the test able to fail.
    const auto recorded = asShotTemperature(*camera);
    const auto effective = temperatureForGains(*camera, camera->appliedMultipliers);
    REQUIRE(std::abs(recorded.kelvin - effective.kelvin) > 1000.0F);

    /// Naming only the tint must leave the temperature alone: the result has to
    /// match spelling out the effective temperature by hand.
    const auto partial = develop(source, {.whiteBalance = WhiteBalanceMode::Custom, .tint = 20.0F});
    const auto spelled = develop(
        source,
        {.whiteBalance = WhiteBalanceMode::Custom, .temperature = effective.kelvin, .tint = 20.0F});

    const auto left = partial.samples<float>();
    const auto right = spelled.samples<float>();
    for (std::size_t index = 0; index < left.size(); ++index) {
        REQUIRE(std::abs(left[index] - right[index]) < 1e-5F);
    }
}

TEST_CASE("Custom with nothing named is the photograph it already was", "[develop]") {
    const auto source = loadImage(test::fixture(skewedNoWbFixture));

    const auto asShot = develop(source, {});
    const auto custom = develop(source, {.whiteBalance = WhiteBalanceMode::Custom});

    const auto left = asShot.samples<float>();
    const auto right = custom.samples<float>();
    for (std::size_t index = 0; index < left.size(); ++index) {
        REQUIRE(left[index] == right[index]);
    }
}
