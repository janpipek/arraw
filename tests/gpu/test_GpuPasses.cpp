#include "GpuDevelop.h"
#include "GpuTesting.h"

#include <DevelopSettings.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

using namespace arraw;
using namespace arraw::test;

namespace {

/// @brief Fills a buffer with the values a render must not disturb.
///
/// What the CPU chain deliberately keeps and a lesser format or converting
/// copy would not: negatives, values above white, signed zero, subnormals, the
/// half-float boundaries and fractional alpha.
ImageBuffer awkwardImage(ImageSize size, ImageOrientation orientation = ImageOrientation::Normal) {
    constexpr std::array colour{
        0.0F,
        -0.0F,
        1.0F,
        -1.0F,
        -0.25F,
        2.0F,
        16.0F,
        65504.0F,
        65520.0F,
        1.0e6F,
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::min(),
        1.0e-40F,
        std::numeric_limits<float>::denorm_min(),
        -std::numeric_limits<float>::denorm_min(),
        1.0e-8F,
    };
    constexpr std::array alpha{0.0F, 1.0F, 0.5F, 1.0F / 3.0F, 1.0F / 255.0F, 0.999999F};

    ImageBuffer image(size, PixelFormat::RgbaF32, workingEncoding, orientation);
    const std::span<float> samples = image.samples<float>();
    constexpr std::size_t channels = 4;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const std::size_t pixel = index / channels;
        samples[index] = index % channels == channels - 1 ? alpha[pixel % alpha.size()]
                                                          : colour[index % colour.size()];
    }
    return image;
}

/// @brief Uploads a buffer and renders the copy pass over it.
DeviceImage copyOf(GpuContext& context, const ImageBuffer& image) {
    return context.render(GpuPass::Copy, {}, context.upload(image), image.size(), image.encoding());
}

} // namespace

TEST_CASE("The copy pass round-trips an RGBA float image bit for bit", "[gpu][passes]") {
    GpuContext& context = gpuContext();
    CAPTURE(sharedGpuContextStatus());

    const ImageBuffer sent = awkwardImage({8, 8});
    const DeviceImage rendered = copyOf(context, sent);
    REQUIRE(rendered.device() == context.id());
    const ImageBuffer received = rendered.readBack();

    REQUIRE(received.size() == sent.size());
    REQUIRE(received.format() == PixelFormat::RgbaF32);
    REQUIRE(received.orientation() == ImageOrientation::Normal);
    const FloatDifference difference = compareFloat(sent, received);
    CAPTURE(difference);
    REQUIRE(difference.bitExact);
}

TEST_CASE("Rendering refuses what it cannot render", "[gpu][passes]") {
    GpuContext& context = gpuContext();
    const ImageBuffer image = awkwardImage({4, 4});
    const DeviceImage input = context.upload(image);

    SECTION("an empty input") {
        REQUIRE_THROWS_AS(context.render(GpuPass::Copy, {}, DeviceImage{}, {4, 4}, workingEncoding),
                          std::invalid_argument);
    }

    SECTION("an image from another context") {
        GpuContext other(gpuTestBackend());
        const DeviceImage foreign = other.upload(image);
        REQUIRE_THROWS_AS(context.render(GpuPass::Copy, {}, foreign, {4, 4}, workingEncoding),
                          std::invalid_argument);
    }

    SECTION("uniforms of the wrong size") {
        const std::array<std::byte, 7> bytes{};
        REQUIRE_THROWS_AS(context.render(GpuPass::Copy, bytes, input, {4, 4}, workingEncoding),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(context.render(GpuPass::Pointwise, {}, input, {4, 4}, workingEncoding),
                          std::invalid_argument);
    }

    SECTION("an empty output size") {
        REQUIRE_THROWS_AS(context.render(GpuPass::Copy, {}, input, {0, 4}, workingEncoding),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(context.render(GpuPass::Copy, {}, input, {4, 0}, workingEncoding),
                          std::invalid_argument);
    }
}

TEST_CASE("A pass can be rendered again and at another size", "[gpu][passes]") {
    GpuContext& context = gpuContext();
    const ImageBuffer image = awkwardImage({6, 5});
    const DeviceImage input = context.upload(image);

    SECTION("twice, reusing its pipeline") {
        for (int round = 0; round < 2; ++round) {
            const ImageBuffer back =
                context.render(GpuPass::Copy, {}, input, image.size(), workingEncoding).readBack();
            REQUIRE(compareFloat(image, back).bitExact);
        }
    }

    SECTION("into a smaller output, which copies the corner") {
        const ImageBuffer back =
            context.render(GpuPass::Copy, {}, input, {3, 2}, workingEncoding).readBack();
        REQUIRE(back.size() == ImageSize{3, 2});
        const std::span<const float> want = image.samples<float>();
        const std::span<const float> got = back.samples<float>();
        for (std::size_t row = 0; row < 2; ++row) {
            for (std::size_t sample = 0; sample < 3 * 4; ++sample) {
                CAPTURE(row, sample);
                REQUIRE(std::bit_cast<std::uint32_t>(got[row * 3 * 4 + sample]) ==
                        std::bit_cast<std::uint32_t>(want[row * 6 * 4 + sample]));
            }
        }
    }
}

TEST_CASE("Developing on the GPU leaves a resident checkpoint", "[gpu][passes]") {
    GpuContext& context = gpuContext();
    const DevelopSettings settings;

    SECTION("stopping after the pointwise pass keeps the source size") {
        const RenderCheckpoint checkpoint =
            developOnGpu(context, awkwardImage({7, 5}), DevelopState{settings}, Stage::Pointwise);
        REQUIRE(checkpoint.isResident());
        REQUIRE(checkpoint.boundary() == Stage::Pointwise);
        REQUIRE(checkpoint.size() == ImageSize{7, 5});
    }

    SECTION("an upright source through geometry keeps its size") {
        const RenderCheckpoint checkpoint =
            developOnGpu(context, awkwardImage({7, 5}), DevelopState{settings}, Stage::Geometry);
        REQUIRE(checkpoint.isResident());
        REQUIRE(checkpoint.boundary() == Stage::Geometry);
        REQUIRE(checkpoint.size() == ImageSize{7, 5});
    }

    SECTION("a quarter-turned source comes out transposed") {
        const RenderCheckpoint checkpoint =
            developOnGpu(context, awkwardImage({7, 5}, ImageOrientation::Rotate90),
                         DevelopState{settings}, Stage::Geometry);
        REQUIRE(checkpoint.isResident());
        REQUIRE(checkpoint.boundary() == Stage::Geometry);
        REQUIRE(checkpoint.size() == ImageSize{5, 7});
    }

    SECTION("every integer and float layout is accepted") {
        for (const PixelFormat format :
             {PixelFormat::RgbU8, PixelFormat::RgbaU16, PixelFormat::RgbaF32}) {
            CAPTURE(static_cast<int>(format));
            const ImageBuffer source({4, 3}, format, workingEncoding);
            const RenderCheckpoint checkpoint =
                developOnGpu(context, source, DevelopState{settings}, Stage::Pointwise);
            REQUIRE(checkpoint.isResident());
            REQUIRE(checkpoint.size() == ImageSize{4, 3});
        }
    }
}
