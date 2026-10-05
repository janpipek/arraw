#include "Denoise.h"
#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuTesting.h"
#include "ProcessingPlan.h"
#include "support/Fixtures.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

using namespace arraw;
using namespace arraw::test;

/// The Denoise pass on the device against the CPU (ADR 039).

namespace {

/// @brief Largest error of the Denoise pass alone against the CPU, relative to the pixel's scale.
///
/// See worstColourError. The spatial weights are the CPU's, uploaded; what is
/// the device's own is the edge-stop's `exp`, the perceptual `pow` (both a
/// few ULP on Vulkan, polynomials on lavapipe), float sums a GPU may fuse,
/// and the division of the ratio decomposition. Measured worst on lavapipe
/// over every case below (levels 0 to 2, odd sizes with partial grid cells,
/// the camera fixture's row, negatives): 8.0e-7. Held to the pointwise pass's bound, as
/// the effects are; a wrong tap, grid cell or bilinear weight disagrees by
/// 1e-3 or more.
constexpr double denoiseRelativeTolerance = pointwiseRelativeTolerance;

/// @brief A portable noise value in [-1, 1) for a pixel and channel.
float noiseAt(std::uint32_t x, std::uint32_t y, std::uint32_t channel) {
    std::uint32_t h = x * 0x9E3779B1U ^ (y + 0x7F4A7C15U) * 0x85EBCA77U ^ channel * 0xC2B2AE3DU;
    h ^= h >> 16;
    h *= 0x7FEB352DU;
    h ^= h >> 15;
    h *= 0x846CA68BU;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / 8388608.0F - 1.0F;
}

/// @brief A step from dark to light halfway across, with independent noise in every channel.
ImageBuffer noisyStep(ImageSize size, float amplitude) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const float base = x < size.width / 2 ? 0.08F : 0.6F;
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = 1.1F * base * (1.0F + amplitude * noiseAt(x, y, 0));
            pixel[1] = base * (1.0F + amplitude * noiseAt(x, y, 1));
            pixel[2] = 0.8F * base * (1.0F + amplitude * noiseAt(x, y, 2));
            pixel[3] = 1.0F;
        }
    }
    return image;
}

DevelopSettings noiseSettings(float luminance, float color, float detail = 50.0F,
                              float smoothness = 50.0F) {
    DevelopSettings settings;
    settings.noiseReduction.luminance = luminance;
    settings.noiseReduction.luminanceDetail = detail;
    settings.noiseReduction.color = color;
    settings.noiseReduction.colorSmoothness = smoothness;
    return settings;
}

/// @brief Requires the device's Denoise boundary to match the CPU's pass within the bound.
void requireDenoiseMatches(const ImageBuffer& original, const DevelopSettings& settings,
                           int level) {
    // A pyramid level's pixels say how many sensor pixels each spans.
    ImageBuffer source = original.clone();
    source.setPixelScale(std::ldexp(1.0, level));
    const DevelopState state{settings};
    const ProcessingPlan plan = planFor(source, state);
    REQUIRE(plan.denoise.active());
    const ImageBuffer expected = applyDenoise(source, plan.denoise);
    const RenderCheckpoint held = developOnGpu(gpuContext(), source, state, Stage::Denoise);
    REQUIRE(held.boundary() == Stage::Denoise);
    REQUIRE(held.encoding() == source.encoding());
    const ImageBuffer actual = held.readBack();
    REQUIRE(actual.size() == expected.size());
    REQUIRE(actual.pixelScale() == expected.pixelScale());
    const double colour = worstColourError(expected, actual);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "denoise level %d: colour %.3g\n", level, colour);
    }
    CAPTURE(level, colour, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    REQUIRE(colour <= denoiseRelativeTolerance);
}

} // namespace

TEST_CASE("The Denoise pass on the device matches the CPU", "[gpu][denoise]") {
    struct Case {
        std::string label;
        DevelopSettings settings;
    };
    const Case cases[]{
        {"luminance alone", noiseSettings(80.0F, 0.0F, 30.0F)},
        {"luminance at the tightest detail", noiseSettings(100.0F, 0.0F, 100.0F)},
        {"colour alone", noiseSettings(0.0F, 70.0F, 50.0F, 40.0F)},
        {"colour at full smoothness", noiseSettings(0.0F, 100.0F, 50.0F, 100.0F)},
        {"both", noiseSettings(60.0F, 50.0F, 20.0F, 70.0F)},
    };
    // An odd size leaves the last grid row and column partly covered.
    const ImageBuffer source = noisyStep({45, 29}, 0.4F);
    for (const Case& entry : cases) {
        for (const int level : {0, 1, 2}) {
            INFO(entry.label << ", level " << level);
            requireDenoiseMatches(source, entry.settings, level);
        }
    }

    SECTION("on a camera's own primaries, through its as-shot row") {
        const ImageBuffer camera = loadImage(fixture("linear-32x24-skewed.dng"));
        requireDenoiseMatches(camera, noiseSettings(60.0F, 80.0F, 40.0F, 60.0F), 0);
    }

    SECTION("negative and near-black channels") {
        ImageBuffer dark = noisyStep({20, 14}, 1.5F);
        const auto samples = dark.samples<float>();
        samples[0] = -0.02F;
        samples[5] = 0.0F;
        samples[10] = 1e-7F;
        requireDenoiseMatches(dark, noiseSettings(70.0F, 90.0F), 0);
    }
}

TEST_CASE("A denoised render on the device matches the CPU end to end", "[gpu][denoise]") {
    const ImageBuffer source = noisyStep({40, 30}, 0.3F);
    DevelopSettings settings = noiseSettings(50.0F, 60.0F);
    settings.tone = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    const DevelopState state{settings};
    const ImageBuffer expected = develop(source, state);
    const ImageBuffer actual = developOnGpu(gpuContext(), source, state).readBack();
    const double colour = worstColourError(expected, actual);
    CAPTURE(colour);
    REQUIRE(colour <= denoiseRelativeTolerance + pointwiseRelativeTolerance);
}

TEST_CASE("The Denoise passes run only when noise reduction is on", "[gpu][denoise]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = noisyStep({40, 30}, 0.3F);
    const auto passesTo = [&](const DevelopSettings& settings, Stage stop) {
        const std::size_t before = context.renderCount();
        static_cast<void>(developOnGpu(context, source, DevelopState{settings}, stop));
        return context.renderCount() - before;
    };
    // Off, whatever Detail and Smoothness say: no pass, and the boundary is the upload.
    const DevelopSettings off = noiseSettings(0.0F, 0.0F, 10.0F, 90.0F);
    REQUIRE(passesTo(off, Stage::Denoise) == 0);
    REQUIRE(passesTo(off, Stage::Pointwise) == passesTo(DevelopSettings{}, Stage::Pointwise));
    REQUIRE(compareFloat(
                source, developOnGpu(context, source, DevelopState{off}, Stage::Denoise).readBack())
                .bitExact);
    // Colour: reduce, blur across, blur down, combine. Luminance: the
    // bilateral's two steps and the combination. Both share the combination.
    REQUIRE(passesTo(noiseSettings(0.0F, 50.0F), Stage::Denoise) == 4);
    REQUIRE(passesTo(noiseSettings(50.0F, 0.0F), Stage::Denoise) == 3);
    REQUIRE(passesTo(noiseSettings(50.0F, 50.0F), Stage::Denoise) == 6);
}

TEST_CASE("A tone edit on the device resumes from the Denoise checkpoint",
          "[gpu][denoise][checkpoint]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = noisyStep({40, 30}, 0.3F);
    DevelopSettings settings = noiseSettings(50.0F, 60.0F);
    const RenderCheckpoint denoised =
        developOnGpu(context, source, DevelopState{settings}, Stage::Denoise);

    settings.tone.exposure = 0.6F;
    const DevelopState after{settings};
    const std::size_t before = context.renderCount();
    const RenderCheckpoint resumed = developOnGpu(context, denoised, source, after, Stage::Effects);
    const std::size_t resumedPasses = context.renderCount() - before;
    const std::size_t freshPasses = [&] {
        const std::size_t start = context.renderCount();
        static_cast<void>(developOnGpu(context, source, after, Stage::Effects));
        return context.renderCount() - start;
    }();
    REQUIRE(resumedPasses + 6 == freshPasses);
    const ImageBuffer fresh = developOnGpu(context, source, after, Stage::Effects).readBack();
    REQUIRE(compareFloat(fresh, resumed.readBack()).bitExact);

    // A noise reduction edit does not resume.
    settings.noiseReduction.color = 80.0F;
    REQUIRE_THROWS_AS(
        developOnGpu(context, denoised, source, DevelopState{settings}, Stage::Pointwise),
        std::invalid_argument);
}

TEST_CASE("A scalar render target keeps the first channel, or falls back to RGBA",
          "[gpu][denoise]") {
    GpuContext& context = gpuContext();
    ImageBuffer source = noisyStep({9, 5}, 0.3F);
    source.setPixelScale(2.0);
    const DeviceImage uploaded = context.upload(source);
    REQUIRE(uploaded.pixelScale() == 2.0);
    const DeviceImage scalar = context.render(GpuPass::Copy, {}, uploaded, source.size(),
                                              source.encoding(), {.format = GpuTargetFormat::R32F});
    REQUIRE(scalar.channelCount() == (context.info().scalarFloatTextures ? 1U : 4U));
    REQUIRE(scalar.pixelScale() == 2.0);
    const ImageBuffer back = scalar.readBack();
    REQUIRE(back.pixelScale() == 2.0);
    const auto in = source.samples<float>();
    const auto out = back.samples<float>();
    for (std::size_t index = 0; index < in.size(); index += 4) {
        REQUIRE(out[index] == in[index]);
        if (scalar.channelCount() == 1) {
            REQUIRE(out[index + 1] == 0.0F);
            REQUIRE(out[index + 2] == 0.0F);
            REQUIRE(out[index + 3] == 1.0F);
        }
    }
    // A pass may say the scale of its result.
    const DeviceImage relabelled = context.render(GpuPass::Copy, {}, uploaded, source.size(),
                                                  source.encoding(), {.pixelScale = 3.0});
    REQUIRE(relabelled.pixelScale() == 3.0);
    REQUIRE(relabelled.channelCount() == 4);
}

TEST_CASE("The device carries the pixel scale as the CPU does", "[gpu][denoise]") {
    GpuContext& context = gpuContext();
    ImageBuffer source = noisyStep({40, 30}, 0.3F);
    source.setPixelScale(2.0);
    const DevelopState state{noiseSettings(40.0F, 40.0F)};
    for (const Stage stop : {Stage::Denoise, Stage::Pointwise, Stage::Geometry, Stage::Effects}) {
        CAPTURE(static_cast<int>(stop));
        const RenderRequest request{.size = RenderRequest::Scale{0.5}};
        const ImageBuffer cpu = developUntil(source, state, stop, request).readBack();
        const ImageBuffer gpu = developOnGpu(context, source, state, stop, request).readBack();
        REQUIRE(gpu.pixelScale() == cpu.pixelScale());
    }
    // After the resize, each pixel spans twice the sensor pixels.
    const RenderRequest half{.size = RenderRequest::Scale{0.5}};
    REQUIRE(developOnGpu(context, source, state, Stage::Effects, half).readBack().pixelScale() ==
            4.0);
}
