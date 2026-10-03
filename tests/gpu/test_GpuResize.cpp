#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuPlan.h"
#include "GpuTesting.h"
#include "ProcessingPlan.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

using namespace arraw;
using namespace arraw::test;

namespace {

/// @brief Settings under which the pointwise chain is the identity on a working-space image.
///
/// So that these tests measure the resize alone: any tone control or the
/// filmic shoulder would add the pointwise pass's own error, and bend the
/// values above one and below zero that the resize cases need to see.
DevelopSettings plainSettings() {
    DevelopSettings settings;
    settings.tone = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    return settings;
}

/// @brief A deterministic pseudo-random value in [0, 1) from three integers.
float hashed(std::uint32_t x, std::uint32_t y, std::uint32_t channel) {
    std::uint32_t h = x * 374761393U + y * 668265263U + channel * 2246822519U + 1013904223U;
    h = (h ^ (h >> 13)) * 1274126177U;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFU) / 16777216.0F;
}

/// @brief Builds an opaque image of smooth gradients with sharp edges and noise.
ImageBuffer opaqueImage(ImageSize size) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * size.width + x) * 4;
            const float u = static_cast<float>(x) / static_cast<float>(size.width);
            const float v = static_cast<float>(y) / static_cast<float>(size.height);
            const bool edge = ((x / 7) + (y / 11)) % 5 == 0; // Hard edges at every scale.
            samples[index] = edge ? 0.95F : 0.1F + 0.5F * u;
            samples[index + 1] = 0.05F + 0.6F * v + 0.2F * hashed(x, y, 1);
            samples[index + 2] = edge ? 0.02F : 0.8F * (1.0F - u) * v + 0.1F * hashed(x, y, 2);
            samples[index + 3] = 1.0F;
        }
    }
    return image;
}

/// @brief Builds an image with a transparent, brightly coloured region beside opaque colour.
///
/// The left third is fully transparent but carries a colour that must not leak,
/// the middle third is partly transparent with varying alpha, the rest is
/// opaque. Rows alternate so that vertical windows meet all three.
ImageBuffer translucentImage(ImageSize size) {
    ImageBuffer image = opaqueImage(size);
    auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * size.width + x) * 4;
            if (x < size.width / 3) {
                samples[index] = 0.9F;
                samples[index + 1] = 0.0F;
                samples[index + 2] = 0.9F;
                samples[index + 3] = (y / 5) % 4 == 0 ? 0.0F : 1.0F;
            } else if (x < 2 * size.width / 3) {
                samples[index + 3] = 0.05F + 0.9F * hashed(x / 2, y / 3, 3);
            }
        }
    }
    return image;
}

/// @brief Builds an opaque image with negative channels and values above one.
ImageBuffer wideRangeImage(ImageSize size) {
    ImageBuffer image = opaqueImage(size);
    auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * size.width + x) * 4;
            samples[index] = 4.0F * hashed(x / 3, y / 3, 4) - 0.5F;       // -0.5 to 3.5
            samples[index + 1] = (x / 9 + y / 9) % 2 == 0 ? -0.3F : 0.4F; // Negative blocks.
            samples[index + 2] = 0.2F + 0.1F * hashed(x, y, 5);           // All positive.
        }
    }
    return image;
}

/// @brief A render done on the device and on the host.
struct Pair {
    ImageBuffer expected; ///< The CPU's result.
    ImageBuffer actual;   ///< The device's result, read back.
};

/// @brief Renders a source with a request on both backends.
Pair renderBoth(const ImageBuffer& source, const DevelopSettings& settings,
                const RenderRequest& request) {
    ImageBuffer expected = develop(source, DevelopState{settings}, request);
    const RenderCheckpoint checkpoint =
        developOnGpu(gpuContext(), source, DevelopState{settings}, Stage::Resize, request);
    REQUIRE(checkpoint.boundary() == Stage::Resize);
    REQUIRE(checkpoint.isResident());
    ImageBuffer actual = checkpoint.readBack();
    REQUIRE(actual.size() == expected.size());
    return {std::move(expected), std::move(actual)};
}

/// @brief How far a device result is from the CPU's.
struct Errors {
    double alpha = 0.0;          ///< Largest absolute alpha difference.
    double colourAbsolute = 0.0; ///< Largest absolute colour difference.
    double colourRelative = 0.0; ///< Largest colour difference relative to max(|expected|, floor).
};

/// @brief Measures the colour of pixels with alpha above a limit, and every alpha.
Errors measure(const Pair& pair, float alphaLimit = 0.0F) {
    const auto want = pair.expected.samples<float>();
    const auto got = pair.actual.samples<float>();
    Errors errors;
    for (std::size_t index = 0; index < want.size(); index += 4) {
        errors.alpha = std::max(errors.alpha, std::abs(static_cast<double>(want[index + 3]) -
                                                       static_cast<double>(got[index + 3])));
        if (want[index + 3] <= alphaLimit) {
            continue;
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double expected = want[index + channel];
            const double difference =
                std::abs(expected - static_cast<double>(got[index + channel]));
            errors.colourAbsolute = std::max(errors.colourAbsolute, difference);
            errors.colourRelative =
                std::max(errors.colourRelative, difference / std::max(std::abs(expected), 1.0e-4));
        }
    }
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "resize: alpha %.3g, colour abs %.3g, rel %.3g\n", errors.alpha,
                     errors.colourAbsolute, errors.colourRelative);
    }
    return errors;
}

/// @brief Requires an opaque or well-conditioned result to match the CPU's.
void requireClose(const Pair& pair) {
    const Errors errors = measure(pair);
    CAPTURE(errors.alpha, errors.colourAbsolute, errors.colourRelative);
    REQUIRE(errors.alpha <= resizeTolerance);
    REQUIRE(errors.colourAbsolute <= resizeTolerance);
}

RenderRequest scaled(double factor, ResizeFilter filter, Upscale upscale = Upscale::Never) {
    return {.size = RenderRequest::Scale{factor}, .upscale = upscale, .filter = filter};
}

RenderRequest fitted(std::uint32_t width, std::uint32_t height, ResizeFilter filter,
                     Upscale upscale = Upscale::Never) {
    return {.size = RenderRequest::FitInside{width, height}, .upscale = upscale, .filter = filter};
}

const char* name(ResizeFilter filter) {
    return filter == ResizeFilter::Lanczos3 ? "Lanczos3" : "Bilinear";
}

} // namespace

TEST_CASE("Resizing on the GPU matches the CPU when shrinking", "[gpu][resize]") {
    const ImageBuffer source = opaqueImage({1200, 800});
    for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        for (const double factor : {0.5, 1.0 / 3.0, 0.125, 1.0 / 13.7, 0.37, 0.9}) {
            DYNAMIC_SECTION(name(filter) << " at " << factor) {
                requireClose(renderBoth(source, plainSettings(), scaled(factor, filter)));
            }
        }
    }
}

TEST_CASE("Resizing on the GPU matches the CPU with a box and awkward sizes", "[gpu][resize]") {
    const ImageBuffer source = opaqueImage({257, 191});
    for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        for (const auto& [width, height] : {std::pair{100U, 100U},
                                            {64U, 1000U},
                                            {1000U, 50U},
                                            {1U, 1U},
                                            {255U, 191U},
                                            {257U, 17U},
                                            {13U, 191U}}) {
            DYNAMIC_SECTION(name(filter) << " into " << width << "x" << height) {
                requireClose(renderBoth(source, plainSettings(), fitted(width, height, filter)));
            }
        }
    }
}

TEST_CASE("Resizing on the GPU matches the CPU when enlarging", "[gpu][resize]") {
    const ImageBuffer source = opaqueImage({61, 43});
    for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        SECTION(std::string(name(filter)) + ", scale 2.5") {
            requireClose(
                renderBoth(source, plainSettings(), scaled(2.5, filter, Upscale::Allowed)));
        }
        SECTION(std::string(name(filter)) + ", box 300x200") {
            requireClose(
                renderBoth(source, plainSettings(), fitted(300, 200, filter, Upscale::Allowed)));
        }
        SECTION(std::string(name(filter)) + ", enlarged across and shrunk down") {
            // A box taller than wide for a wide image changes one side only in kind.
            requireClose(renderBoth(source, plainSettings(),
                                    {.size = RenderRequest::FitInside{122, 20},
                                     .upscale = Upscale::Allowed,
                                     .filter = filter}));
        }
        SECTION(std::string(name(filter)) + ", capped without upscaling") {
            const Pair pair = renderBoth(source, plainSettings(), scaled(3.0, filter));
            CHECK(pair.actual.size() == source.size());
            requireClose(pair);
        }
    }
}

TEST_CASE("Resizing on the GPU matches the CPU beside transparency", "[gpu][resize]") {
    const ImageBuffer source = translucentImage({300, 200});
    for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        for (const double factor : {0.5, 0.2, 1.0 / 7.3, 2.3}) {
            DYNAMIC_SECTION(name(filter) << " at " << factor) {
                const Pair pair = renderBoth(
                    source, plainSettings(),
                    scaled(factor, filter, factor > 1.0 ? Upscale::Allowed : Upscale::Never));
                const Errors errors = measure(pair, resizeVisibleAlpha);
                CAPTURE(errors.alpha, errors.colourAbsolute, errors.colourRelative);
                REQUIRE(errors.alpha <= resizeTolerance);
                REQUIRE(errors.colourAbsolute <= illConditionedResizeTolerance);
            }
        }
    }
}

TEST_CASE("Resizing on the GPU keeps a transparent region clean", "[gpu][resize]") {
    // Fully transparent left half, opaque right half: nothing of the left
    // half's colour may reach the result, and a fully transparent output
    // pixel has all four samples zero.
    ImageBuffer source = opaqueImage({200, 100});
    auto samples = source.samples<float>();
    for (std::uint32_t y = 0; y < 100; ++y) {
        for (std::uint32_t x = 0; x < 100; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * 200 + x) * 4;
            samples[index] = 5.0F;
            samples[index + 1] = 5.0F;
            samples[index + 2] = 5.0F;
            samples[index + 3] = 0.0F;
        }
    }
    for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        DYNAMIC_SECTION(name(filter)) {
            const Pair pair = renderBoth(source, plainSettings(), scaled(0.21, filter));
            const auto want = pair.expected.samples<float>();
            const auto got = pair.actual.samples<float>();
            std::size_t transparentOutputs = 0;
            for (std::size_t index = 0; index < want.size(); index += 4) {
                if (want[index + 3] == 0.0F) {
                    ++transparentOutputs;
                    for (std::size_t c = 0; c < 4; ++c) {
                        CHECK(got[index + c] == 0.0F);
                    }
                }
                for (std::size_t c = 0; c < 3; ++c) {
                    if (want[index + 3] > resizeVisibleAlpha) {
                        // Colour is the opaque side's, with a little overshoot, never the 5 of the
                        // clear side.
                        CHECK(got[index + c] <= 1.2F);
                    }
                }
            }
            CHECK(transparentOutputs > 0);
            const Errors errors = measure(pair, resizeVisibleAlpha);
            CHECK(errors.alpha <= resizeTolerance);
            CHECK(errors.colourAbsolute <= illConditionedResizeTolerance);
        }
    }
}

TEST_CASE("Resizing on the GPU matches the CPU on negative and bright channels", "[gpu][resize]") {
    const ImageBuffer source = wideRangeImage({400, 300});
    for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        for (const double factor : {0.5, 1.0 / 8.0, 0.23, 1.7}) {
            DYNAMIC_SECTION(name(filter) << " at " << factor) {
                const Pair pair = renderBoth(
                    source, plainSettings(),
                    scaled(factor, filter, factor > 1.0 ? Upscale::Allowed : Upscale::Never));
                const Errors errors = measure(pair);
                CAPTURE(errors.alpha, errors.colourAbsolute, errors.colourRelative);
                REQUIRE(errors.alpha <= resizeTolerance);
                REQUIRE(errors.colourAbsolute <= illConditionedResizeTolerance);
            }
        }
    }
}

TEST_CASE("Resizing on the GPU rings no lower than black in a non-negative image",
          "[gpu][resize]") {
    // A hard black and white edge: Lanczos undershoots, and the rule clamps it.
    ImageBuffer source({120, 40}, workingFormat, workingEncoding);
    auto samples = source.samples<float>();
    for (std::size_t index = 0; index < samples.size(); index += 4) {
        const bool bright = (index / 4) % 120 >= 60;
        samples[index] = samples[index + 1] = samples[index + 2] = bright ? 1.0F : 0.0F;
        samples[index + 3] = 1.0F;
    }
    const Pair pair = renderBoth(source, plainSettings(), scaled(0.31, ResizeFilter::Lanczos3));
    for (const float sample : pair.actual.samples<float>()) {
        CHECK(sample >= 0.0F);
    }
    requireClose(pair);
}

TEST_CASE("Resizing on the GPU matches the CPU after rotation and a crop", "[gpu][resize]") {
    const ImageBuffer source = translucentImage({240, 180});
    const std::pair<const char*, GeometrySettings> cases[] = {
        {"straighten and auto crop", {.straighten = 7.5}},
        {"quarter-turn, flip, straighten",
         {.rotation = QuarterTurn::Clockwise90, .flipHorizontal = true, .straighten = -12.0}},
        {"explicit crop",
         {.straighten = 3.0,
          .crop = {.rectangle =
                       UprightCropRect{.left = 0.1, .top = 0.2, .right = 0.8, .bottom = 0.9}}}},
    };
    for (const auto& [label, geometry] : cases) {
        for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
            DYNAMIC_SECTION(label << ", " << name(filter)) {
                DevelopSettings settings = plainSettings();
                settings.geometry = geometry;
                const Pair pair = renderBoth(source, settings, scaled(0.37, filter));
                const Errors errors = measure(pair, resizeVisibleAlpha);
                CAPTURE(errors.alpha, errors.colourAbsolute, errors.colourRelative);
                REQUIRE(errors.alpha <= resizeTolerance);
                REQUIRE(errors.colourAbsolute <= illConditionedResizeTolerance);
            }
        }
    }
}

TEST_CASE("Resizing on the GPU follows the pointwise chain", "[gpu][resize]") {
    // The default settings run the chain, shoulder included, and then the resize
    // from the same plan.
    const ImageBuffer source = opaqueImage({150, 100});
    const Pair pair = renderBoth(source, DevelopSettings{}, scaled(0.4, ResizeFilter::Lanczos3));
    const Errors errors = measure(pair);
    // The pointwise error is relative to the pixel, whose scale is at most one here.
    CHECK(errors.alpha <= resizeTolerance);
    CHECK(errors.colourAbsolute <= pointwiseRelativeTolerance + resizeTolerance);
}

TEST_CASE("An identity request leaves the geometry checkpoint unchanged", "[gpu][resize]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = translucentImage({40, 30});
    DevelopSettings settings = plainSettings();
    settings.geometry = {.straighten = 10.0};

    const RenderCheckpoint geometry =
        developOnGpu(context, source, DevelopState{settings}, Stage::Geometry);
    const ImageBuffer framed = geometry.readBack();
    const ImageSize cropped = geometry.size();

    const RenderRequest identities[] = {
        {},
        scaled(1.0, ResizeFilter::Lanczos3),
        scaled(4.0, ResizeFilter::Lanczos3), // Capped at one without Upscale::Allowed.
        fitted(cropped.width, cropped.height, ResizeFilter::Bilinear),
        fitted(cropped.width * 3, cropped.height * 3, ResizeFilter::Lanczos3),
    };
    for (const RenderRequest& request : identities) {
        const RenderCheckpoint resized =
            developOnGpu(context, source, DevelopState{settings}, Stage::Resize, request);
        CHECK(resized.boundary() == Stage::Resize);
        CHECK(resized.size() == cropped);
        CHECK(resized.isResident());
        CHECK(compareFloat(framed, resized.readBack()).bitExact);
    }
}

TEST_CASE("Stopping after the geometry ignores the requested size", "[gpu][resize]") {
    const ImageBuffer source = opaqueImage({64, 48});
    const RenderCheckpoint checkpoint =
        developOnGpu(gpuContext(), source, DevelopState{plainSettings()}, Stage::Geometry,
                     scaled(0.5, ResizeFilter::Lanczos3));
    CHECK(checkpoint.boundary() == Stage::Geometry);
    CHECK(checkpoint.size() == source.size());

    const RenderCheckpoint resized =
        developOnGpu(gpuContext(), source, DevelopState{plainSettings()}, Stage::Resize,
                     scaled(0.5, ResizeFilter::Lanczos3));
    CHECK(resized.boundary() == Stage::Resize);
    CHECK(resized.size() == ImageSize{32, 24});
}

TEST_CASE("Resizing on the GPU refuses a request that cannot be resolved", "[gpu][resize]") {
    // Outside the checks: without a device it skips, which a check would take
    // for the wrong exception.
    GpuContext& context = gpuContext();
    const ImageBuffer source = opaqueImage({16, 16});
    CHECK_THROWS_AS(developOnGpu(context, source, DevelopState{plainSettings()}, Stage::Resize,
                                 scaled(0.0, ResizeFilter::Lanczos3)),
                    std::invalid_argument);
    CHECK_THROWS_AS(developOnGpu(context, source, DevelopState{plainSettings()}, Stage::Resize,
                                 fitted(0, 10, ResizeFilter::Lanczos3)),
                    std::invalid_argument);
}

TEST_CASE("The resize weights are laid out as the shaders read them", "[gpu][resize]") {
    // 10 -> 4 with Lanczos: radius 3 * 2.5 = 7.5, so up to 15 taps, 4 texels of weights.
    const ImageBuffer weights = packResizeWeights(10, 4, ResizeFilter::Lanczos3);
    REQUIRE(weights.size().height == 4);
    const auto samples = weights.samples<float>();
    const std::size_t width = weights.size().width;
    for (std::size_t row = 0; row < 4; ++row) {
        const float* texels = &samples[row * width * 4];
        const auto count = static_cast<std::size_t>(texels[1]);
        REQUIRE(count >= 1);
        REQUIRE(1 + (count + 3) / 4 <= width);
        double sum = 0.0;
        for (std::size_t k = 0; k < count; ++k) {
            sum += texels[4 + k];
        }
        CHECK(std::abs(sum - 1.0) < 1e-6);
    }
    // At one to one every output has the single source pixel of its own index.
    const ImageBuffer identity = packResizeWeights(5, 5, ResizeFilter::Lanczos3);
    const auto ones = identity.samples<float>();
    for (std::size_t row = 0; row < 5; ++row) {
        const float* texels = &ones[row * identity.size().width * 4];
        CHECK(texels[0] == static_cast<float>(row));
        CHECK(texels[1] == 1.0F);
        CHECK(texels[4] == 1.0F);
    }
}

TEST_CASE("Resizing an opaque image on the GPU gives exactly opaque pixels", "[gpu][resize]") {
    // The weights sum to 1 only to within a float ulp, yet the JPEG export
    // accepts nothing but alpha == 1.0F.
    const ImageBuffer source = opaqueImage({1200, 800});
    for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        DYNAMIC_SECTION(name(filter)) {
            const Pair pair = renderBoth(source, plainSettings(), fitted(444, 296, filter));
            const auto expected = pair.expected.samples<float>();
            const auto actual = pair.actual.samples<float>();
            std::size_t inexact = 0;
            for (std::size_t i = 3; i < actual.size(); i += 4) {
                if (actual[i] != 1.0F || expected[i] != 1.0F) {
                    ++inexact;
                }
            }
            CHECK(inexact == 0);
        }
    }
}

TEST_CASE("The resize passes refuse a wrong number of inputs", "[gpu][resize]") {
    GpuContext& context = gpuContext();
    const DeviceImage image = context.upload(opaqueImage({8, 8}));
    const GpuResizeBlock block{.plane = 0, .inputLength = 8};
    const auto bytes = std::as_bytes(std::span(&block, 1));
    const std::array one{image};
    for (const GpuPass pass : {GpuPass::ResizeAcross, GpuPass::ResizeDown}) {
        CHECK_THROWS_AS(context.render(pass, bytes, one, ImageSize{4, 4}, workingEncoding),
                        std::invalid_argument);
    }
}

TEST_CASE("Resizing an opaque image on the GPU takes two renders, not four",
          "[gpu][resize][opaque]") {
    GpuContext& context = gpuContext();
    const RenderRequest request = scaled(0.4, ResizeFilter::Lanczos3);
    const auto rendersFor = [&](const ImageBuffer& source, const RenderRequest& asked) {
        const std::size_t before = context.renderCount();
        (void)developOnGpu(context, source, DevelopState{plainSettings()}, Stage::Resize, asked);
        return context.renderCount() - before;
    };

    // The pointwise pass, then the resize: one row pass, one column pass.
    const ImageBuffer opaque = opaqueImage({300, 200});
    CHECK(planFor(opaque, DevelopState{plainSettings()}, request).resize->opaque);
    CHECK(rendersFor(opaque, request) == 1 + 2);

    // The general path: three planes, then the column pass.
    ImageBuffer nearlyOpaque = opaqueImage({300, 200});
    nearlyOpaque.samples<float>()[3] = std::nextafter(1.0F, 0.0F);
    CHECK_FALSE(planFor(nearlyOpaque, DevelopState{plainSettings()}, request).resize->opaque);
    CHECK(rendersFor(nearlyOpaque, request) == 1 + 4);
    CHECK(rendersFor(translucentImage({300, 200}), request) == 1 + 4);

    // No resize, no passes for it, opaque or not.
    CHECK(rendersFor(opaque, {}) == 1);
    CHECK(rendersFor(nearlyOpaque, {}) == 1);

    // A geometry pass in front counts once more, and does not change the rest.
    DevelopSettings rotated = plainSettings();
    rotated.geometry.straighten = 5.0;
    const std::size_t before = context.renderCount();
    (void)developOnGpu(context, opaque, DevelopState{rotated}, Stage::Resize, request);
    CHECK(context.renderCount() - before == 1 + 1 + 2);
}

TEST_CASE("The opaque resize passes agree with the general ones on an opaque image",
          "[gpu][resize][opaque]") {
    GpuContext& context = gpuContext();
    for (const ImageBuffer& source : {opaqueImage({211, 137}), wideRangeImage({211, 137})}) {
        for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
            for (const ImageSize target :
                 {ImageSize{50, 31}, ImageSize{211, 20}, ImageSize{400, 300}}) {
                DYNAMIC_SECTION(name(filter) << " to " << target.width << "x" << target.height) {
                    const DeviceImage image = context.upload(source);
                    const ImageSize size = source.size();
                    const DeviceImage acrossWeights =
                        context.upload(packResizeWeights(size.width, target.width, filter));
                    const DeviceImage downWeights =
                        context.upload(packResizeWeights(size.height, target.height, filter));
                    const ImageSize widened{target.width, size.height};
                    const auto bytes = [](const GpuResizeBlock& block) {
                        return std::as_bytes(std::span(&block, 1));
                    };

                    std::array<DeviceImage, resizePlaneCount> planes;
                    for (std::size_t plane = 0; plane < resizePlaneCount; ++plane) {
                        const GpuResizeBlock block{.plane = static_cast<std::uint32_t>(plane),
                                                   .inputLength = size.width};
                        planes[plane] = context.render(GpuPass::ResizeAcross, bytes(block),
                                                       std::array{image, acrossWeights}, widened,
                                                       workingEncoding);
                    }
                    const GpuResizeBlock down{.plane = 0, .inputLength = size.height};
                    const ImageBuffer general =
                        context
                            .render(GpuPass::ResizeDown, bytes(down),
                                    std::array{planes[0], downWeights, planes[1], planes[2]},
                                    target, workingEncoding)
                            .readBack();

                    const GpuResizeBlock across{.plane = 0, .inputLength = size.width};
                    const DeviceImage sums =
                        context.render(GpuPass::ResizeAcrossOpaque, bytes(across),
                                       std::array{image, acrossWeights}, widened, workingEncoding);
                    const ImageBuffer opaque =
                        context
                            .render(GpuPass::ResizeDownOpaque, bytes(down),
                                    std::array{sums, downWeights}, target, workingEncoding)
                            .readBack();

                    const FloatDifference difference = compareFloat(general, opaque);
                    CAPTURE(difference);
                    CHECK(difference.maxAbsDiff <= resizeTolerance);
                    for (std::size_t i = 3; i < opaque.samples<float>().size(); i += 4) {
                        REQUIRE(opaque.samples<float>()[i] == 1.0F);
                    }
                    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
                        std::fprintf(stderr,
                                     "opaque vs general passes: max abs %.3g, bit exact %d\n",
                                     difference.maxAbsDiff, difference.bitExact ? 1 : 0);
                    }
                }
            }
        }
    }
}

TEST_CASE("Resizing an opaque image on the GPU matches the CPU after rotation and a crop",
          "[gpu][resize][opaque]") {
    const ImageBuffer source = opaqueImage({240, 180});
    // The default crop of a straightened image runs to the edge of the content,
    // where an interpolation may reach the border.
    const std::pair<const char*, GeometrySettings> cases[] = {
        {"interior crop",
         {.straighten = 7.5,
          .crop = {.rectangle =
                       UprightCropRect{.left = 0.1, .top = 0.2, .right = 0.8, .bottom = 0.9}}}},
        {"straighten and auto crop", {.straighten = 7.5}},
        {"quarter-turn, flip, straighten",
         {.rotation = QuarterTurn::Clockwise90, .flipHorizontal = true, .straighten = -12.0}},
    };
    for (const auto& [label, geometry] : cases) {
        for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
            DYNAMIC_SECTION(label << ", " << name(filter)) {
                DevelopSettings settings = plainSettings();
                settings.geometry = geometry;
                const Pair pair = renderBoth(source, settings, scaled(0.37, filter));
                REQUIRE(
                    planFor(source, DevelopState{settings}, scaled(0.37, filter)).resize->opaque);
                requireClose(pair);
                for (std::size_t i = 3; i < pair.actual.samples<float>().size(); i += 4) {
                    REQUIRE(pair.actual.samples<float>()[i] == 1.0F);
                    REQUIRE(pair.expected.samples<float>()[i] == 1.0F);
                }
            }
        }
    }
}

namespace {

/// @brief A request for a part of the frame at a scale.
RenderRequest regional(RenderRequest::Region region, double factor, ResizeFilter filter,
                       Upscale upscale = Upscale::Never) {
    RenderRequest request = scaled(factor, filter, upscale);
    request.region = region;
    return request;
}

} // namespace

TEST_CASE("Rendering a region on the GPU matches the CPU", "[gpu][resize][region]") {
    const ImageBuffer opaque = opaqueImage({400, 300});
    const ImageBuffer translucent = translucentImage({400, 300});
    const RenderRequest::Region regions[] = {
        {0.25, 0.25, 0.75, 0.75}, {0.0, 0.0, 0.3, 0.4}, {0.61, 0.5, 1.0, 1.0}};
    for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        for (const RenderRequest::Region& region : regions) {
            for (const double factor : {0.4, 1.0, 2.5}) {
                DYNAMIC_SECTION(name(filter) << " region " << region.left << "," << region.top
                                             << " at " << factor) {
                    const Upscale upscale = factor > 1.0 ? Upscale::Allowed : Upscale::Never;
                    const auto request = regional(region, factor, filter, upscale);
                    requireClose(renderBoth(opaque, plainSettings(), request));
                    const Pair pair = renderBoth(translucent, plainSettings(), request);
                    const Errors errors = measure(pair, 0.01F);
                    CHECK(errors.alpha <= resizeTolerance);
                    CHECK(errors.colourAbsolute <= resizeTolerance);
                }
            }
        }
    }
}

TEST_CASE("Rendering a region on the GPU matches the CPU after a rotation",
          "[gpu][resize][region]") {
    const ImageBuffer source = opaqueImage({300, 200});
    DevelopSettings settings = plainSettings();
    settings.geometry = {.rotation = QuarterTurn::Clockwise90, .straighten = 7.0};
    requireClose(
        renderBoth(source, settings, regional({0.1, 0.2, 0.6, 0.9}, 0.5, ResizeFilter::Lanczos3)));
}

TEST_CASE("A geometry checkpoint on the GPU is resumed across regions", "[gpu][resize][region]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = opaqueImage({200, 150});
    const DevelopState state{plainSettings()};
    const RenderCheckpoint geometry = developOnGpu(context, source, state, Stage::Geometry);

    for (const RenderRequest::Region region :
         {RenderRequest::Region{0.0, 0.0, 0.5, 0.5}, {0.5, 0.5, 1.0, 1.0}, {0.2, 0.1, 0.9, 0.6}}) {
        const RenderRequest request = regional(region, 0.5, ResizeFilter::Lanczos3);
        const RenderCheckpoint resumed =
            developOnGpu(context, geometry, source, state, Stage::Resize, request);
        REQUIRE(resumed.isResident());
        const ImageBuffer expected = develop(source, state, request);
        REQUIRE(resumed.size() == expected.size());
        requireClose({expected.clone(), resumed.readBack()});
    }
}
