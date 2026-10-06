#include "SampleConversion.h"

#include <ImageBuffer.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

using namespace arraw;

TEST_CASE("Pixel format descriptions", "[ImageBuffer]") {
    STATIC_REQUIRE(channelCount(PixelFormat::RgbU8) == 3);
    STATIC_REQUIRE(channelCount(PixelFormat::RgbaF32) == 4);

    STATIC_REQUIRE(bytesPerChannel(PixelFormat::RgbU8) == 1);
    STATIC_REQUIRE(bytesPerChannel(PixelFormat::RgbaU16) == 2);
    STATIC_REQUIRE(bytesPerChannel(PixelFormat::RgbF32) == 4);

    STATIC_REQUIRE(bytesPerPixel(PixelFormat::RgbU8) == 3);
    STATIC_REQUIRE(bytesPerPixel(PixelFormat::RgbaU8) == 4);
    STATIC_REQUIRE(bytesPerPixel(PixelFormat::RgbU16) == 6);
    STATIC_REQUIRE(bytesPerPixel(PixelFormat::RgbaF32) == 16);
}

TEST_CASE("Image size", "[ImageBuffer]") {
    STATIC_REQUIRE(ImageSize{}.empty());
    STATIC_REQUIRE(ImageSize{4, 0}.empty());
    STATIC_REQUIRE(ImageSize{0, 4}.empty());
    STATIC_REQUIRE_FALSE(ImageSize{4, 4}.empty());

    SECTION("pixel count is widened before multiplying") {
        constexpr ImageSize large{100'000, 100'000};
        STATIC_REQUIRE(large.pixelCount() == 10'000'000'000ULL);
        STATIC_REQUIRE(large.pixelCount() > std::numeric_limits<std::uint32_t>::max());
    }

    SECTION("equality is memberwise") {
        REQUIRE(ImageSize{6, 4} == ImageSize{6, 4});
        REQUIRE(ImageSize{6, 4} != ImageSize{4, 6});
    }
}

TEST_CASE("A new buffer reports its geometry and starts zeroed", "[ImageBuffer]") {
    const ImageBuffer buffer({6, 4}, PixelFormat::RgbaF32, NamedEncoding::LinearRec2020);

    REQUIRE(buffer.size() == ImageSize{6, 4});
    REQUIRE(buffer.format() == PixelFormat::RgbaF32);
    REQUIRE(isWorkingEncoding(buffer.encoding()));

    SECTION("rows are tightly packed") {
        REQUIRE(buffer.rowStride() == 6 * 4 * sizeof(float));
        REQUIRE(buffer.byteSize() == buffer.rowStride() * 4);
    }

    SECTION("storage holds one sample per channel per pixel") {
        const auto samples = buffer.samples<float>();
        REQUIRE(samples.size() == 6 * 4 * 4);
        REQUIRE(std::ranges::all_of(samples, [](float value) { return value == 0.0F; }));
    }
}

TEST_CASE("Sample storage is allocated per format", "[ImageBuffer]") {
    SECTION("8-bit") {
        const ImageBuffer buffer({2, 2}, PixelFormat::RgbU8, NamedEncoding::Srgb);
        REQUIRE(buffer.samples<std::uint8_t>().size() == 2 * 2 * 3);
        REQUIRE(buffer.byteSize() == 12);
    }

    SECTION("16-bit") {
        const ImageBuffer buffer({2, 2}, PixelFormat::RgbaU16, NamedEncoding::AdobeRgb);
        REQUIRE(buffer.samples<std::uint16_t>().size() == 2 * 2 * 4);
        REQUIRE(buffer.byteSize() == 32);
    }

    SECTION("asking for the wrong sample type is an error, not a reinterpretation") {
        const ImageBuffer buffer({2, 2}, PixelFormat::RgbU8, NamedEncoding::Srgb);
        REQUIRE_THROWS_AS(buffer.samples<float>(), std::bad_variant_access);
    }
}

TEST_CASE("Byte and sample views address the same storage", "[ImageBuffer]") {
    ImageBuffer buffer({2, 1}, PixelFormat::RgbF32, NamedEncoding::LinearRec2020);

    const auto samples = buffer.samples<float>();
    samples[0] = 0.25F;
    samples[5] = 0.75F;

    const auto bytes = buffer.bytes();
    REQUIRE(bytes.size() == buffer.byteSize());
    REQUIRE(static_cast<const void*>(bytes.data()) ==
            static_cast<const void*>(buffer.samples<float>().data()));

    SECTION("writes through one view are visible through the other") {
        float first = 0.0F;
        std::memcpy(&first, bytes.data(), sizeof(float));
        REQUIRE(first == 0.25F);
    }

    SECTION("a const buffer yields const views") {
        const ImageBuffer& readOnly = buffer;
        STATIC_REQUIRE(std::is_same_v<decltype(readOnly.bytes()), std::span<const std::byte>>);
        STATIC_REQUIRE(std::is_same_v<decltype(readOnly.samples<float>()), std::span<const float>>);
    }
}

TEST_CASE("Degenerate dimensions are rejected", "[ImageBuffer]") {
    REQUIRE_THROWS_AS(ImageBuffer({0, 4}, PixelFormat::RgbU8, NamedEncoding::Srgb),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(ImageBuffer({4, 0}, PixelFormat::RgbU8, NamedEncoding::Srgb),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(ImageBuffer({0, 0}, PixelFormat::RgbU8, NamedEncoding::Srgb),
                      std::invalid_argument);
}

TEST_CASE("An unrepresentable allocation fails before it is attempted", "[ImageBuffer]") {
    // 4e9 x 4e9 RGBA pixels is 6.4e19 samples, past the range of size_t.
    constexpr ImageSize absurd{4'000'000'000, 4'000'000'000};
    REQUIRE_THROWS_AS(ImageBuffer(absurd, PixelFormat::RgbaU8, NamedEncoding::Srgb),
                      std::length_error);
}

TEST_CASE("Cloning produces an independent buffer", "[ImageBuffer]") {
    ImageBuffer original({2, 2}, PixelFormat::RgbF32, NamedEncoding::LinearRec2020);
    original.samples<float>()[0] = 1.5F;

    ImageBuffer copy = original.clone();

    REQUIRE(copy.size() == original.size());
    REQUIRE(copy.format() == original.format());
    REQUIRE(copy.encoding() == original.encoding());
    REQUIRE(copy.rowStride() == original.rowStride());
    REQUIRE(copy.samples<float>()[0] == 1.5F);

    SECTION("the copy does not alias the original") {
        copy.samples<float>()[0] = 0.5F;
        REQUIRE(original.samples<float>()[0] == 1.5F);
        REQUIRE(copy.samples<float>().data() != original.samples<float>().data());
    }
}

TEST_CASE("Buffers move rather than copy", "[ImageBuffer]") {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<ImageBuffer>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<ImageBuffer>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<ImageBuffer>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<ImageBuffer>);

    ImageBuffer source({2, 2}, PixelFormat::RgbF32, NamedEncoding::LinearRec2020);
    source.samples<float>()[3] = 2.5F;
    const float* const storage = source.samples<float>().data();

    const ImageBuffer moved = std::move(source);

    REQUIRE(moved.size() == ImageSize{2, 2});
    REQUIRE(moved.samples<float>().data() == storage);
    REQUIRE(moved.samples<float>()[3] == 2.5F);
}

TEST_CASE("A buffer carries its pixel scale through copies", "[ImageBuffer]") {
    ImageBuffer buffer({4, 2}, PixelFormat::RgbaU16, workingEncoding);
    REQUIRE(buffer.pixelScale() == 1.0);
    buffer.setPixelScale(2.0);
    REQUIRE(buffer.clone().pixelScale() == 2.0);
    REQUIRE(toRgbaF32(buffer).pixelScale() == 2.0);
    const ImageBuffer moved = std::move(buffer);
    REQUIRE(moved.pixelScale() == 2.0);
    ImageBuffer other({1, 1}, PixelFormat::RgbaF32, workingEncoding);
    for (const double bad : {0.0, -2.0, std::numeric_limits<double>::infinity(), std::nan("")}) {
        REQUIRE_THROWS_AS(other.setPixelScale(bad), std::invalid_argument);
    }
    REQUIRE(other.pixelScale() == 1.0);
}

TEST_CASE("A large buffer of every format starts all zero", "[ImageBuffer][threads]") {
    const PixelFormat format =
        GENERATE(PixelFormat::RgbU8, PixelFormat::RgbaU8, PixelFormat::RgbU16, PixelFormat::RgbaU16,
                 PixelFormat::RgbF32, PixelFormat::RgbaF32);
    // Enough pixels for several bands, and an odd height so the bands are uneven.
    constexpr ImageSize size{1021, 513};
    {
        // Memory freed dirty, which the allocator may hand out again: a fresh mapping
        // would be zero from the kernel, and the test could not fail. Freeing a large
        // block raises glibc's mmap threshold, so the next one comes from the heap.
        ImageBuffer dirty(size, format, NamedEncoding::Srgb);
        std::ranges::fill(dirty.bytes(), std::byte{0xFF});
        ImageBuffer again(size, format, NamedEncoding::Srgb);
        std::ranges::fill(again.bytes(), std::byte{0xFF});
    }
    const ImageBuffer buffer(size, format, NamedEncoding::Srgb);

    REQUIRE(buffer.byteSize() == std::size_t{size.width} * size.height * bytesPerPixel(format));
    REQUIRE(
        std::ranges::all_of(buffer.bytes(), [](std::byte value) { return value == std::byte{0}; }));
}
