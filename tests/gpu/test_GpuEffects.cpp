#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuTesting.h"
#include "GrainModels.h"
#include "ProcessingPlan.h"
#include "support/TestImages.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// The Effects pass on the device against the CPU (ADR 037, ADR 038).

namespace {

/// @brief Largest error of the effects pass alone against the CPU, relative to the pixel's scale.
///
/// See worstColourError. Darkening is one `exp2` and a multiply; lightening
/// adds two powers per channel, through the perceptual coordinate and back,
/// which is where GLSL's `pow` and `exp2` (a few ULP on Vulkan, a polynomial
/// on lavapipe) part from the CPU's correctly rounded ones. Grain adds the
/// same round trip; its lattices are integer and single-rounded float work,
/// the same on both (ADR 038). Measured worst on lavapipe over every case
/// below, with and without a resize: 1.0e-6 for the vignette, 1.6e-6 with
/// grain. Held to
/// the pointwise pass's bound, whose powers are the same and whose margin
/// covers drivers less exact than lavapipe; a wrong falloff, position or
/// branch disagrees by 1e-3 or more.
constexpr double effectsRelativeTolerance = pointwiseRelativeTolerance;

/// @brief Effects after a resize: the resize's error, then the effects'.
constexpr double effectsAfterResizeTolerance = effectsRelativeTolerance + resampleTolerance;

/// @brief Settings that leave the pointwise chain exact on the device, with a vignette.
DevelopSettings vignetted(float amount, float midpoint = 50.0F, float feather = 50.0F) {
    DevelopSettings settings;
    settings.tone = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    settings.effects.vignette = {amount, midpoint, feather};
    return settings;
}

/// @brief Requires the device's render of a request to match the CPU's within a bound.
void requireMatchesCpu(const ImageBuffer& source, const DevelopSettings& settings,
                       const RenderRequest& request, double tolerance) {
    const DevelopState state{settings};
    const ImageBuffer expected = develop(source, state, request);
    const ImageBuffer actual =
        developOnGpu(gpuContext(), source, state, Stage::Effects, request).readBack();
    REQUIRE(actual.size() == expected.size());
    const double colour = worstColourError(expected, actual);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "effects: colour %.3g\n", colour);
    }
    CAPTURE(colour, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    REQUIRE(colour <= tolerance);
}

/// @brief Settings that leave the pointwise chain exact on the device, with grain.
DevelopSettings grainy(float amount, float size, float roughness, std::uint32_t seed) {
    DevelopSettings settings = vignetted(0.0F);
    settings.effects.grain = {.amount = amount, .size = size, .roughness = roughness, .seed = seed};
    return settings;
}

/// @brief A working-space sweep with values above white and below zero, for the lightening branch.
ImageBuffer widenedSweep(ImageSize size) {
    ImageBuffer image = rainbow(size, PixelFormat::RgbaF32, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::size_t index = 0; index < samples.size(); index += 4) {
        samples[index] = samples[index] * 1.6F - 0.1F;
        samples[index + 2] = samples[index + 2] * 2.5F;
    }
    return image;
}

} // namespace

TEST_CASE("The effects pass matches the CPU", "[gpu][effects]") {
    const ImageBuffer source = widenedSweep({61, 41});
    struct Case {
        const char* label;
        DevelopSettings settings;
    };
    const std::vector<Case> cases{
        {"darkening, soft", vignetted(-70.0F)},
        {"darkening from the centre", vignetted(-40.0F, 0.0F, 100.0F)},
        {"lightening, soft", vignetted(80.0F, 60.0F, 40.0F)},
        {"lightening, full", vignetted(100.0F, 0.0F, 100.0F)},
    };
    for (const Case& c : cases) {
        INFO(c.label);
        requireMatchesCpu(source, c.settings, {}, effectsRelativeTolerance);
    }
}

TEST_CASE("The hard-edged vignette matches the CPU away from its edge", "[gpu][effects]") {
    // A feather of 0 is a step at the inner radius: a pixel within an ulp of it
    // may fall on either side on a device whose square root or position part
    // from the CPU's. Such a pixel is allowed to flip; every other one is held
    // to the usual bound, and a wrong edge flips far more than a couple.
    const ImageBuffer source = widenedSweep({61, 41});
    const DevelopState state{vignetted(-100.0F, 30.0F, 0.0F)};
    const ImageBuffer expected = develop(source, state, {});
    const ImageBuffer actual =
        developOnGpu(gpuContext(), source, state, Stage::Effects, {}).readBack();
    REQUIRE(actual.size() == expected.size());
    const auto want = expected.samples<float>();
    const auto got = actual.samples<float>();
    std::size_t flipped = 0;
    for (std::size_t pixel = 0; pixel + 3 < want.size(); pixel += 4) {
        double worst = 0.0;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const double scale =
                std::max(1.0, std::abs(static_cast<double>(want[pixel + channel])));
            worst = std::max(worst, std::abs(static_cast<double>(want[pixel + channel]) -
                                             static_cast<double>(got[pixel + channel])) /
                                        scale);
        }
        if (worst > effectsRelativeTolerance) {
            ++flipped;
        }
    }
    CAPTURE(flipped);
    // Pixels mirrored across the centre share a radius to the ulp, so a driver
    // that rounds the edge differently can flip four at once; a wrong edge
    // would flip whole rings.
    REQUIRE(flipped <= 8);
}

TEST_CASE("The effects pass matches the CPU after a region and a resize",
          "[gpu][effects][region]") {
    const ImageBuffer source = widenedSweep({61, 41});
    const std::vector<RenderRequest> requests{
        {.size = RenderRequest::FitInside{23, 23}},
        {.region = RenderRequest::Region{0.2, 0.1, 0.9, 0.6}},
        {.size = RenderRequest::Scale{2.5},
         .region = RenderRequest::Region{0.5, 0.5, 1.0, 1.0},
         .upscale = Upscale::Allowed},
    };
    for (const RenderRequest& request : requests) {
        requireMatchesCpu(source, vignetted(-60.0F, 25.0F, 70.0F), request,
                          effectsAfterResizeTolerance);
        requireMatchesCpu(source, vignetted(55.0F), request, effectsAfterResizeTolerance);
    }
}

TEST_CASE("Grain on the device matches the CPU", "[gpu][effects][grain]") {
    // Enlarged, so that grain at every size is coarse enough to be drawn: a
    // 61-pixel sweep eight times over is a 488-pixel frame.
    const ImageBuffer source = widenedSweep({61, 41});
    const RenderRequest enlarged{.size = RenderRequest::Scale{8.0}, .upscale = Upscale::Allowed};
    struct Case {
        const char* label;
        DevelopSettings settings;
    };
    DevelopSettings both = grainy(70.0F, 60.0F, 50.0F, 99U);
    both.effects.vignette = {-50.0F, 30.0F, 60.0F};
    DevelopSettings lifted = grainy(40.0F, 100.0F, 20.0F, 5U);
    lifted.effects.vignette = {80.0F, 50.0F, 50.0F};
    const std::vector<Case> cases{
        {"fine and even", grainy(100.0F, 0.0F, 0.0F, 1U)},
        {"middling", grainy(50.0F, 50.0F, 50.0F, 0xfeedfaceU)},
        {"coarse and clustered", grainy(100.0F, 100.0F, 100.0F, 0U)},
        {"after a darkening vignette", both},
        {"after a lightening vignette", lifted},
    };
    for (const Case& c : cases) {
        INFO(c.label);
        requireMatchesCpu(source, c.settings, enlarged, effectsAfterResizeTolerance);
    }
}

TEST_CASE("Grain on the device matches the CPU in a region, shrunk and far in",
          "[gpu][effects][grain][region]") {
    const DevelopSettings settings = grainy(80.0F, 30.0F, 60.0F, 1234U);
    const ImageBuffer sweep = widenedSweep({256, 192});
    const std::vector<RenderRequest> requests{
        {},
        {.region = RenderRequest::Region{0.2, 0.1, 0.9, 0.6}},
        {.size = RenderRequest::FitInside{100, 100}},
        // A frame 4096 pixels wide, seen through a window of 256 by 192: the
        // lattices hold thousands of cells, placed on the host in double.
        {.size = RenderRequest::Scale{16.0},
         .region = RenderRequest::Region{0.75, 0.75, 0.8125, 0.8125},
         .upscale = Upscale::Allowed},
    };
    for (const RenderRequest& request : requests) {
        requireMatchesCpu(sweep, settings, request, effectsAfterResizeTolerance);
    }
    // The frame at its own size and shrunk is too small for the finest
    // lattice, so those renders draw the pixel-scale substitute too.
    const ProcessingPlan shrunk = planFor(sweep, DevelopState{settings}, requests[2]);
    REQUIRE(grainPlacementOf(shrunk.effects.grain, frameMappingOf(shrunk))
                .layers[valueNoiseSubstituteLayer]
                .weight > 0.0F);
}

TEST_CASE("The effects pass runs only when an effect is on", "[gpu][effects]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = rainbow({40, 30}, PixelFormat::RgbaF32, workingEncoding);
    const RenderRequest request{.size = RenderRequest::FitInside{20, 20}};
    const auto passesTo = [&](const DevelopSettings& settings, Stage stop) {
        const std::size_t before = context.renderCount();
        static_cast<void>(developOnGpu(context, source, DevelopState{settings}, stop, request));
        return context.renderCount() - before;
    };
    const DevelopSettings plain = vignetted(0.0F, 10.0F, 0.0F);
    REQUIRE(passesTo(plain, Stage::Effects) == passesTo(plain, Stage::Resize));
    const DevelopSettings on = vignetted(-30.0F);
    REQUIRE(passesTo(on, Stage::Effects) == passesTo(on, Stage::Resize) + 1);
    const DevelopSettings grain = grainy(30.0F, 50.0F, 50.0F, 8U);
    REQUIRE(passesTo(grain, Stage::Effects) == passesTo(grain, Stage::Resize) + 1);
    // Grain with no amount is off, whatever its size, roughness or seed.
    const DevelopSettings noGrain = grainy(0.0F, 90.0F, 10.0F, 8U);
    REQUIRE(passesTo(noGrain, Stage::Effects) == passesTo(noGrain, Stage::Resize));

    // Off, the effects boundary is the resize's pixels, bit for bit.
    const DevelopState state{plain};
    const ImageBuffer resized =
        developOnGpu(context, source, state, Stage::Resize, request).readBack();
    const ImageBuffer effects =
        developOnGpu(context, source, state, Stage::Effects, request).readBack();
    REQUIRE(compareFloat(resized, effects).bitExact);
}

TEST_CASE("A vignette edit on the GPU resumes from the resize in one pass",
          "[gpu][effects][checkpoint]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = rainbow({40, 30}, PixelFormat::RgbaF32, workingEncoding);
    const RenderRequest request{.size = RenderRequest::FitInside{20, 20}};
    const RenderCheckpoint resized =
        developOnGpu(context, source, DevelopState{vignetted(-30.0F)}, Stage::Resize, request);

    const DevelopState after{vignetted(45.0F, 70.0F, 20.0F)};
    const std::size_t before = context.renderCount();
    const RenderCheckpoint resumed =
        developOnGpu(context, resized, source, after, Stage::Effects, request);
    REQUIRE(context.renderCount() == before + 1);
    REQUIRE(resumed.boundary() == Stage::Effects);
    const ImageBuffer fresh =
        developOnGpu(context, source, after, Stage::Effects, request).readBack();
    REQUIRE(compareFloat(fresh, resumed.readBack()).bitExact);
}
