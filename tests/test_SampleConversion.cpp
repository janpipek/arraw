#include "SampleConversion.h"

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

using namespace arraw;
using Catch::Approx;

TEST_CASE("Sample conversion scales integers by their maximum", "[SampleConversion]") {
    STATIC_REQUIRE(toUnit<std::uint8_t>(255) == 1.0F);
    STATIC_REQUIRE(toUnit<std::uint8_t>(0) == 0.0F);
    STATIC_REQUIRE(toUnit<std::uint16_t>(65535) == 1.0F);
    STATIC_REQUIRE(toUnit(-0.5F) == -0.5F);
    REQUIRE(toUnit<std::uint8_t>(51) == Approx(0.2F));
}

TEST_CASE("An 8-bit RGB buffer converts to RGBA float with opaque alpha", "[SampleConversion]") {
    ImageBuffer source({2, 1}, PixelFormat::RgbU8, NamedEncoding::Srgb, ImageOrientation::Rotate90);
    const std::array<std::uint8_t, 6> values{0, 51, 255, 255, 0, 102};
    std::ranges::copy(values, source.samples<std::uint8_t>().begin());

    const ImageBuffer converted = toRgbaF32(source);

    REQUIRE(converted.format() == PixelFormat::RgbaF32);
    REQUIRE(converted.size() == source.size());
    REQUIRE(converted.encoding() == source.encoding());
    REQUIRE(converted.orientation() == ImageOrientation::Rotate90);
    const std::span<const float> samples = converted.samples<float>();
    REQUIRE(samples.size() == 8);
    REQUIRE(samples[0] == 0.0F);
    REQUIRE(samples[1] == Approx(0.2F));
    REQUIRE(samples[2] == 1.0F);
    REQUIRE(samples[3] == 1.0F);
    REQUIRE(samples[4] == 1.0F);
    REQUIRE(samples[5] == 0.0F);
    REQUIRE(samples[6] == Approx(0.4F));
    REQUIRE(samples[7] == 1.0F);
}

TEST_CASE("A 16-bit RGBA buffer keeps its own alpha, scaled", "[SampleConversion]") {
    ImageBuffer source({1, 1}, PixelFormat::RgbaU16, NamedEncoding::AdobeRgb,
                       ImageOrientation::MirrorHorizontal);
    const std::array<std::uint16_t, 4> values{65535, 0, 32768, 16384};
    std::ranges::copy(values, source.samples<std::uint16_t>().begin());

    const ImageBuffer converted = toRgbaF32(source);

    REQUIRE(converted.format() == PixelFormat::RgbaF32);
    REQUIRE(converted.encoding() == source.encoding());
    REQUIRE(converted.orientation() == ImageOrientation::MirrorHorizontal);
    const std::span<const float> samples = converted.samples<float>();
    REQUIRE(samples[0] == 1.0F);
    REQUIRE(samples[1] == 0.0F);
    REQUIRE(samples[2] == Approx(32768.0F / 65535.0F));
    REQUIRE(samples[3] == Approx(16384.0F / 65535.0F));
}

TEST_CASE("A 16-bit RGB buffer gains opaque alpha", "[SampleConversion]") {
    ImageBuffer source({1, 1}, PixelFormat::RgbU16, NamedEncoding::Srgb);
    const std::array<std::uint16_t, 3> values{65535, 0, 65535};
    std::ranges::copy(values, source.samples<std::uint16_t>().begin());

    const ImageBuffer converted = toRgbaF32(source);
    const std::span<const float> samples = converted.samples<float>();
    REQUIRE(samples[0] == 1.0F);
    REQUIRE(samples[1] == 0.0F);
    REQUIRE(samples[2] == 1.0F);
    REQUIRE(samples[3] == 1.0F);
}
