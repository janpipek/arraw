#include "Develop.h"
#include "ImageImport.h"

#include "support/Fixtures.h"
#include "support/TestImages.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

using namespace arraw;

namespace {

constexpr std::string_view neutralFixture = "linear-32x24-neutral.dng";
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
