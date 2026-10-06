#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuTesting.h"
#include "Presence.h"
#include "ProcessingPlan.h"
#include "support/Fixtures.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using namespace arraw;
using namespace arraw::test;

/// Texture, Clarity and Dehaze on the device against the CPU (ADR 041).

namespace {

/// @brief Largest error of the pointwise pass with Presence against the CPU, relative to the
/// pixel's scale.
///
/// See worstColourError. The context's weights are the CPU's, uploaded; what is
/// the device's own is `log2` in the reduction and in each pixel's log
/// luminance, `exp2` of the gain, float sums a GPU may fuse, and the
/// pointwise chain's powers. A log2 error of a few ULP of values near -14 to 0
/// is an absolute error of about 1e-6 stops in the detail, which the gain
/// turns into a relative error of the same size. Measured worst on lavapipe
/// over every case below: see the test's printout (ARRAW_PRINT_MEASURED);
/// held to the pointwise bound. A wrong cell, tap or bilinear weight
/// disagrees by 1e-3 or more.
constexpr double presenceRelativeTolerance = pointwiseRelativeTolerance;

/// @brief A tinted scene of smooth shapes and fine detail.
ImageBuffer sceneOf(ImageSize size) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double shapes = 0.6 * std::sin(x / 5.0) * std::cos(y / 4.0) +
                                  0.3 * std::sin((x + y) / 11.0) +
                                  0.08 * std::sin(x / 1.3) * std::sin(y / 1.7);
            const auto value = static_cast<float>(0.2 * std::exp2(shapes));
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = 1.15F * value;
            pixel[1] = value;
            pixel[2] = 0.75F * value;
            pixel[3] = 1.0F;
        }
    }
    return image;
}

DevelopSettings presence(float texture, float clarity, float dehaze) {
    DevelopSettings settings;
    settings.presence = {.texture = texture, .clarity = clarity, .dehaze = dehaze};
    return settings;
}

/// @brief Requires the device's pointwise boundary to match the CPU's within the bound.
double requirePointwiseMatches(ImageBuffer source, const DevelopSettings& settings,
                               double pixelScale) {
    source.setPixelScale(pixelScale);
    const DevelopState state{settings};
    REQUIRE(planFor(source, state).presence.active());
    const ImageBuffer expected = developUntil(source, state, Stage::Pointwise).readBack();
    const ImageBuffer actual =
        developOnGpu(gpuContext(), source, state, Stage::Pointwise).readBack();
    const double colour = worstColourError(expected, actual);
    CAPTURE(pixelScale, colour, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    REQUIRE(colour <= presenceRelativeTolerance);
    return colour;
}

} // namespace

TEST_CASE("Presence on the device matches the CPU", "[gpu][presence]") {
    struct Case {
        std::string label;
        DevelopSettings settings;
    };
    const Case cases[]{
        {"texture", presence(70.0F, 0.0F, 0.0F)},
        {"negative texture", presence(-100.0F, 0.0F, 0.0F)},
        {"clarity", presence(0.0F, 100.0F, 0.0F)},
        {"negative clarity", presence(0.0F, -60.0F, 0.0F)},
        {"dehaze", presence(0.0F, 0.0F, 100.0F)},
        {"negative dehaze", presence(0.0F, 0.0F, -80.0F)},
        {"all three, with tone",
         [] {
             DevelopSettings settings = presence(40.0F, 60.0F, 30.0F);
             settings.tone.exposure = 0.4F;
             settings.tone.contrast = 30.0F;
             return settings;
         }()},
    };
    // An odd size leaves the last grid row and column partly covered; the
    // long edge of 203 makes Clarity's cell one pixel at scale 1 and two at 4.
    const ImageBuffer source = sceneOf({203, 77});
    double worst = 0.0;
    for (const Case& entry : cases) {
        for (const double scale : {1.0, 2.0, 4.0, 8.0}) {
            INFO(entry.label << ", pixel scale " << scale);
            worst = std::max(worst, requirePointwiseMatches(source.clone(), entry.settings, scale));
        }
    }

    SECTION("on a camera's own primaries, through its as-shot row") {
        const ImageBuffer camera = loadImage(fixture("linear-32x24-skewed.dng"));
        worst = std::max(
            worst, requirePointwiseMatches(camera.clone(), presence(50.0F, 80.0F, 40.0F), 1.0));
    }

    SECTION("a wide source, whose Clarity cells are many pixels") {
        worst = std::max(worst, requirePointwiseMatches(sceneOf({1203, 33}),
                                                        presence(30.0F, 70.0F, 50.0F), 1.0));
        worst = std::max(
            worst, requirePointwiseMatches(sceneOf({1203, 33}), presence(0.0F, 0.0F, -70.0F), 1.0));
    }

    SECTION("an export's cells: eight pixels, with Dehaze's opening over many") {
        const ImageBuffer wide = sceneOf({3203, 24});
        REQUIRE(
            planFor(wide, DevelopState{presence(0.0F, 50.0F, 50.0F)}).presence.coarse.reduction ==
            8);
        worst = std::max(worst,
                         requirePointwiseMatches(wide.clone(), presence(30.0F, 70.0F, 60.0F), 1.0));
    }

    SECTION("a bright disk, whose opening the reconstruction brings back to its outline") {
        // 600 pixels: cells of one, an octagon of inradius 18 (8 across, 5 a
        // diagonal); a disk of 60 pixels under a veil, which holds the octagon
        // only by its middle.
        ImageBuffer disk(ImageSize{600, 400}, workingFormat, workingEncoding);
        const auto samples = disk.samples<float>();
        for (std::uint32_t y = 0; y < 400; ++y) {
            for (std::uint32_t x = 0; x < 600; ++x) {
                const bool inside = std::hypot(x - 300.3, y - 200.7) < 60.0;
                const float value = 0.6F * (inside ? 0.3F : 0.03F) + 0.2F;
                float* pixel = &samples[(static_cast<std::size_t>(y) * 600 + x) * 4];
                pixel[0] = 1.1F * value;
                pixel[1] = value;
                pixel[2] = 0.8F * value;
                pixel[3] = 1.0F;
            }
        }
        const PresencePlan plan =
            planFor(disk, DevelopState{presence(0.0F, 0.0F, 100.0F)}).presence;
        REQUIRE(plan.haze.window == 18);
        REQUIRE(plan.haze.reconstruction == hazeReconstructionSteps);
        worst = std::max(worst,
                         requirePointwiseMatches(disk.clone(), presence(0.0F, 0.0F, 100.0F), 1.0));
    }

    SECTION("a checkerboard of cells, which the octagon's diagonal passes split by parity") {
        // 203 pixels: cells of one, an octagon of inradius 6, 2 across and 2 a
        // diagonal. Each diagonal pass reads the cells of one parity only.
        ImageBuffer board(ImageSize{203, 77}, workingFormat, workingEncoding);
        const auto samples = board.samples<float>();
        for (std::uint32_t y = 0; y < 77; ++y) {
            for (std::uint32_t x = 0; x < 203; ++x) {
                const float value = (x + y) % 2 == 0 ? 0.02F : 0.3F;
                float* pixel = &samples[(static_cast<std::size_t>(y) * 203 + x) * 4];
                pixel[0] = value;
                pixel[1] = value;
                pixel[2] = value;
                pixel[3] = 1.0F;
            }
        }
        const PresencePlan plan =
            planFor(board, DevelopState{presence(0.0F, 0.0F, 100.0F)}).presence;
        REQUIRE(octagonOf(plan.haze.window) == OctagonWindow{.across = 2, .diagonal = 2});
        worst = std::max(worst,
                         requirePointwiseMatches(board.clone(), presence(0.0F, 0.0F, 100.0F), 1.0));
    }

    SECTION("a non-finite pixel, bounded the same way") {
        ImageBuffer broken = sceneOf({203, 77});
        const auto samples = broken.samples<float>();
        for (std::size_t channel = 0; channel < 3; ++channel) {
            samples[(20 * 203 + 30) * 4 + channel] = std::numeric_limits<float>::infinity();
            samples[(40 * 203 + 150) * 4 + channel] = std::numeric_limits<float>::quiet_NaN();
        }
        for (const float dehaze : {60.0F, -60.0F}) {
            worst = std::max(worst, requirePointwiseMatches(broken.clone(),
                                                            presence(50.0F, 80.0F, dehaze), 1.0));
        }
    }
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "presence worst colour error: %.3g\n", worst);
    }
}

TEST_CASE("A Presence render on the device matches the CPU end to end", "[gpu][presence]") {
    const ImageBuffer source = sceneOf({120, 80});
    DevelopSettings settings = presence(30.0F, 50.0F, 40.0F);
    settings.noiseReduction.color = 40.0F;
    settings.geometry.straighten = 2.0;
    const DevelopState state{settings};
    const RenderRequest request{.size = RenderRequest::Scale{0.5}};
    const ImageBuffer expected = develop(source, state, request);
    const ImageBuffer actual =
        developOnGpu(gpuContext(), source, state, Stage::Effects, request).readBack();
    const double colour = worstColourError(expected, actual);
    CAPTURE(colour);
    REQUIRE(colour <= presenceRelativeTolerance + resampleTolerance);
}

TEST_CASE("The Presence passes run only for the bases the controls read", "[gpu][presence]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = sceneOf({60, 40});
    const auto passesFor = [&](const DevelopSettings& settings) {
        const std::size_t before = context.renderCount();
        static_cast<void>(developOnGpu(context, source, DevelopState{settings}, Stage::Pointwise));
        return context.renderCount() - before;
    };
    const std::size_t plain = passesFor(DevelopSettings{});
    REQUIRE(plain == 1);
    REQUIRE(passesFor(presence(0.0F, 0.0F, 0.0F)) == plain);
    // Texture and Clarity: reduce, blur across, blur down. Dehaze shares
    // Clarity's reduction and adds its blur, and a positive one the octagon's
    // opening (the minimum and the maximum, each across, down and along both
    // diagonals, even where the diagonal is zero, as here with a window of
    // two cells) and its reconstruction, a render a step, before it: a fixed
    // count of two.
    const PresencePlan clearer =
        planFor(source, DevelopState{presence(0.0F, 0.0F, 50.0F)}).presence;
    REQUIRE(octagonOf(clearer.haze.window).diagonal == 0);
    const std::size_t steps = clearer.haze.reconstruction;
    REQUIRE(steps == hazeReconstructionSteps);
    REQUIRE(passesFor(presence(50.0F, 0.0F, 0.0F)) == plain + 3);
    REQUIRE(passesFor(presence(0.0F, 50.0F, 0.0F)) == plain + 3);
    REQUIRE(passesFor(presence(0.0F, 0.0F, -50.0F)) == plain + 3);
    REQUIRE(passesFor(presence(0.0F, 0.0F, 50.0F)) == plain + 11 + steps);
    REQUIRE(passesFor(presence(0.0F, 50.0F, -50.0F)) == plain + 5);
    REQUIRE(passesFor(presence(0.0F, 50.0F, 50.0F)) == plain + 13 + steps);
    REQUIRE(passesFor(presence(50.0F, 50.0F, 50.0F)) == plain + 16 + steps);

    // At an export's size the count is the same: 13 renders for a positive
    // Dehaze, whatever the window.
    const ImageBuffer wide = sceneOf({3203, 24});
    const std::size_t before = context.renderCount();
    static_cast<void>(
        developOnGpu(context, wide, DevelopState{presence(0.0F, 0.0F, 50.0F)}, Stage::Pointwise));
    REQUIRE(context.renderCount() - before == plain + 13);

    // Off, the pixels are those of a render without the controls, bit for bit.
    REQUIRE(compareFloat(
                developOnGpu(context, source, DevelopState{DevelopSettings{}}).readBack(),
                developOnGpu(context, source, DevelopState{presence(0.0F, 0.0F, 0.0F)}).readBack())
                .bitExact);
}

TEST_CASE("A Presence edit on the device resumes from the Denoise checkpoint",
          "[gpu][presence][checkpoint]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = sceneOf({64, 48});
    DevelopSettings settings = presence(20.0F, 40.0F, 0.0F);
    settings.noiseReduction.color = 60.0F;
    const RenderCheckpoint denoised =
        developOnGpu(context, source, DevelopState{settings}, Stage::Denoise);

    settings.presence.clarity = -50.0F;
    settings.presence.dehaze = 30.0F;
    settings.tone.exposure = 0.3F;
    const DevelopState after{settings};
    const std::size_t before = context.renderCount();
    const RenderCheckpoint resumed = developOnGpu(context, denoised, source, after, Stage::Effects);
    const std::size_t resumedPasses = context.renderCount() - before;
    const std::size_t start = context.renderCount();
    const ImageBuffer fresh = developOnGpu(context, source, after, Stage::Effects).readBack();
    const std::size_t freshPasses = context.renderCount() - start;
    // The colour half of noise reduction is four renders; the context is
    // recomputed from the checkpoint either way.
    REQUIRE(resumedPasses + 4 == freshPasses);
    REQUIRE(compareFloat(fresh, resumed.readBack()).bitExact);
}

TEST_CASE("The curve input on the device includes Presence", "[gpu][presence][sample]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = sceneOf({80, 60});
    const DevelopState state{presence(40.0F, 70.0F, 30.0F)};
    const ImageBuffer cpu = sample(source, state, Tap::CurveInput);
    const ImageBuffer gpu = sampleOnGpu(context, source, state, Tap::CurveInput);
    // Compared after decoding back to linear, as the tap's own parity test does.
    const auto decode = [](ImageBuffer image) {
        for (float& value : image.samples<float>()) {
            value = fromPerceptualSigned(value);
        }
        return image;
    };
    const double colour = worstColourError(decode(cpu.clone()), decode(gpu.clone()));
    CAPTURE(colour);
    REQUIRE(colour <= presenceRelativeTolerance);
    REQUIRE_FALSE(compareFloat(cpu, sample(source, DevelopState{}, Tap::CurveInput)).bitExact);
}
