#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuPlan.h"
#include "GpuTesting.h"
#include "Lens.h"
#include "ProcessingPlan.h"
#include "support/LadderTesting.h"
#include "support/TestImages.h"

#include <CheckpointLadder.h>
#include <Develop.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <LensCorrection.h>
#include <Progress.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// The Lens pass on the device against the CPU's (ADR 050, section 9).

namespace {

/// @brief Largest absolute error of the lens pass alone against the CPU, on the scenes below.
///
/// The scenes hold values from -0.3 to 3 with hard edges, so this is an absolute error in
/// working-space units. The CPU positions a sample in double and the device in float offsets
/// from the optical centre (GpuLensBlock), so what differs is a few thousandths of a pixel in
/// where the Catmull-Rom taps fall, times the scene's steepest edge, and float sums of 16 taps.
/// Measured worst on lavapipe over every case below: 3.8e-6 (a strong barrel on 97x61), about
/// 6 float ulps of the scene's largest values; set ARRAW_PRINT_MEASURED to print them. Kept at
/// 2e-5 for drivers less exact than lavapipe. A wrong centre, fill, radius, table or kernel
/// disagrees by 1e-2 or more.
constexpr double lensTolerance = 2.0e-5;

/// @brief Largest error of a develop with lens corrections, relative to the pixel's scale.
///
/// Measured worst on lavapipe: 5.0e-7, with the chain's own bound the larger.
constexpr double lensDevelopTolerance = pointwiseRelativeTolerance;

/// @brief Largest position error, in pixels, on a linear ramp image whose value is its column.
///
/// On the ramp a Catmull-Rom read is exact, so the difference between the two backends' values
/// is the difference in where they read. Measured worst on lavapipe on 6000 pixels wide: 3.4e-3
/// (a pincushion). It cannot show less than the float rounding of values
/// up to 6000 (one ulp is 5e-4) summed over sixteen taps, which is most of what is measured.
constexpr double positionTolerance = 5.0e-3;

/// @brief Which corrections a test lens carries.
struct Kinds {
    double distortion = 0.0; ///< Change of the radial scale at the corner; zero for none.
    double lateralCa = 0.0;  ///< Change of red's scale at the corner, blue's the opposite.
    double vignetting = 0.0; ///< Gain added at the corner; zero for none.
    double centerU = 0.5;    ///< Optical centre across.
    double centerV = 0.5;    ///< Optical centre down.
};

/// @brief Makes a correction of the kinds asked for, its fill scale set.
LensCorrection lensOf(const Kinds& kinds, double aspect) {
    LensCorrection correction;
    correction.aspect = aspect;
    correction.centerU = kinds.centerU;
    correction.centerV = kinds.centerV;
    const auto radial = [](double strength) {
        std::vector<float> nodes(lensTableNodes);
        for (std::size_t node = 0; node < lensTableNodes; ++node) {
            const double radius = static_cast<double>(node) / (lensTableNodes - 1);
            // A cubic term too, so that the table is not a parabola a coarse mistake would fit.
            nodes[node] = static_cast<float>(
                1.0 + strength * (radius * radius + 0.3 * radius * radius * radius));
        }
        return nodes;
    };
    if (kinds.distortion != 0.0) {
        correction.distortion = radial(kinds.distortion);
    }
    if (kinds.lateralCa != 0.0) {
        correction.redScale = radial(kinds.lateralCa);
        correction.blueScale = radial(-0.7 * kinds.lateralCa);
    }
    if (kinds.vignetting != 0.0) {
        correction.vignetting = radial(kinds.vignetting);
    }
    return withFillScale(std::move(correction));
}

/// @brief Makes a scene with hard edges, smooth gradients, values above one and below zero.
ImageBuffer scene(ImageSize size) {
    ImageBuffer image(size, PixelFormat::RgbaF32, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double u = (x + 0.5) / size.width;
            const double v = (y + 0.5) / size.height;
            const bool bar = (x / 5 + y / 7) % 2 == 0;
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = static_cast<float>(0.1 + 0.8 * u + (bar ? 0.9 : 0.0));
            pixel[1] = static_cast<float>(0.05 + 0.6 * v + (bar ? 0.3 : 0.0));
            pixel[2] = static_cast<float>(1.2 * std::sin(6.0 * u + 3.0 * v) + 0.2);
            pixel[3] = 1.0F;
        }
    }
    // Some pixels far above white, below zero, and translucent.
    samples[0] = 3.0F;
    samples[1] = -0.3F;
    if (size.width > 4) {
        samples[4 * 4 + 3] = 0.4F;
    }
    return image;
}

/// @brief Makes an image whose red, green and blue are the column, so a read gives the position.
ImageBuffer ramp(ImageSize size) {
    ImageBuffer image(size, PixelFormat::RgbaF32, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = static_cast<float>(x);
            pixel[1] = static_cast<float>(x);
            pixel[2] = static_cast<float>(x);
            pixel[3] = 1.0F;
        }
    }
    return image;
}

/// @brief Largest absolute difference over all four channels.
double worstAbsolute(const ImageBuffer& expected, const ImageBuffer& actual) {
    return compareFloat(expected, actual).maxAbsDiff;
}

/// @brief Requires the device's Lens stage to match the CPU's within a bound.
double requireMatchesCpu(const ImageBuffer& source, const LensCorrection& lens, double tolerance,
                         const char* label) {
    const DevelopState state;
    const ImageBuffer expected = developUntil(source, state, lens, Stage::Lens).readBack();
    const RenderCheckpoint onDevice = developOnGpu(gpuContext(), source, state, lens, Stage::Lens);
    REQUIRE(onDevice.isResident());
    const ImageBuffer actual = onDevice.readBack();
    REQUIRE(actual.size() == expected.size());
    const double worst = worstAbsolute(expected, actual);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "lens %s: %.3g\n", label, worst);
    }
    CAPTURE(label, compareFloat(expected, actual));
    REQUIRE(worst <= tolerance);
    return worst;
}

} // namespace

TEST_CASE("The lens block and tables are the plan's", "[gpu][lens][plan]") {
    LensPlan plan;
    plan.correction = lensOf({.distortion = 0.06,
                              .lateralCa = 0.004,
                              .vignetting = 0.5,
                              .centerU = 0.52,
                              .centerV = 0.47},
                             1.5);
    const GpuLensBlock block = packLens(plan, {600, 400});
    CHECK(block.distorts == 1);
    CHECK(block.colorSplit == 1);
    CHECK(block.dims == 1);
    CHECK(block.centerWhole[0] == 312.0F);
    CHECK(block.centerWhole[1] == 188.0F);
    CHECK(block.centerFraction[0] == 0.0F);
    CHECK(block.centerFraction[1] == 0.0F);
    CHECK(block.lastNode == static_cast<float>(lensTableNodes - 1));
    CHECK(block.inverseFill * static_cast<float>(plan.correction.fill) == Catch::Approx(1.0F));
    CHECK(block.fillGain == Catch::Approx(block.inverseFill - 1.0F));
    CHECK(block.fillGain < 0.0F);

    const ImageBuffer tables = packLensTables(plan);
    REQUIRE(tables.size() == ImageSize{static_cast<std::uint32_t>(lensTableNodes), 1});
    const auto samples = tables.samples<float>();
    for (const std::size_t node : {std::size_t{0}, std::size_t{37}, lensTableNodes - 1}) {
        CHECK(samples[node * 4] ==
              static_cast<float>(static_cast<double>(plan.correction.distortion[node]) - 1.0));
        CHECK(samples[node * 4 + 1] ==
              static_cast<float>(static_cast<double>(plan.correction.redScale[node]) - 1.0));
        CHECK(samples[node * 4 + 2] ==
              static_cast<float>(static_cast<double>(plan.correction.blueScale[node]) - 1.0));
        CHECK(samples[node * 4 + 3] == plan.correction.vignetting[node]);
    }

    // A correction with one kind packs only that kind.
    LensPlan dimming;
    dimming.correction = lensOf({.vignetting = 0.3}, 1.5);
    const GpuLensBlock dims = packLens(dimming, {600, 400});
    CHECK(dims.distorts == 0);
    CHECK(dims.colorSplit == 0);
    CHECK(dims.dims == 1);
    CHECK(dims.inverseFill == 1.0F);
    CHECK(dims.fillGain == 0.0F);
}

TEST_CASE("The lens pass matches the CPU, each correction and all of them", "[gpu][lens]") {
    struct Case {
        const char* label;
        Kinds kinds;
    };
    const std::vector<Case> cases{
        {"barrel", {.distortion = -0.08}},
        {"pincushion", {.distortion = 0.06}},
        {"red and blue", {.lateralCa = 0.004}},
        {"vignetting", {.vignetting = 0.8}},
        {"barrel, red and blue", {.distortion = -0.08, .lateralCa = 0.004}},
        {"red and blue, vignetting", {.lateralCa = 0.004, .vignetting = 0.8}},
        {"all three", {.distortion = -0.05, .lateralCa = -0.003, .vignetting = 0.6}},
        {"all three, off centre",
         {.distortion = 0.04,
          .lateralCa = 0.005,
          .vignetting = 0.4,
          .centerU = 0.56,
          .centerV = 0.43}},
        {"a strong barrel", {.distortion = -0.25, .vignetting = 0.2}},
    };
    for (const ImageSize size : {ImageSize{61, 41}, ImageSize{97, 61}, ImageSize{33, 64}}) {
        const double sizeAspect = static_cast<double>(size.width) / size.height;
        const ImageBuffer source = scene(size);
        for (const Case& c : cases) {
            const std::string label = std::string(c.label) + " " + std::to_string(size.width) +
                                      "x" + std::to_string(size.height);
            // The tables are for the aspect of the frame they are applied to.
            requireMatchesCpu(source, lensOf(c.kinds, sizeAspect), lensTolerance, label.c_str());
        }
    }
}

TEST_CASE("The lens pass reads the positions the CPU does at the size of a photograph",
          "[gpu][lens]") {
    // The value of the ramp is the column it is read at, so the difference is in pixels.
    const ImageSize size{6000, 64};
    const ImageBuffer source = ramp(size);
    const double aspect = static_cast<double>(size.width) / size.height;
    struct Case {
        const char* label;
        Kinds kinds;
    };
    const std::vector<Case> cases{
        {"barrel", {.distortion = -0.08}},
        {"pincushion", {.distortion = 0.03}},
        {"red and blue", {.lateralCa = 0.004}},
        {"all, off centre",
         {.distortion = -0.05,
          .lateralCa = 0.003,
          .vignetting = 0.3,
          .centerU = 0.53,
          .centerV = 0.48}},
    };
    for (const Case& c : cases) {
        const LensCorrection lens = lensOf(c.kinds, aspect);
        const DevelopState state;
        const ImageBuffer expected = developUntil(source, state, lens, Stage::Lens).readBack();
        const ImageBuffer actual =
            developOnGpu(gpuContext(), source, state, lens, Stage::Lens).readBack();
        // The vignetting gain scales the ramp, so it is divided out at the same radius where
        // there is one: compare the positions only where the correction is a pure move.
        if (c.kinds.vignetting != 0.0) {
            const double worst = compareFloat(expected, actual).maxRelDiff;
            if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
                std::fprintf(stderr, "lens ramp %s: relative %.3g\n", c.label, worst);
            }
            CAPTURE(c.label);
            CHECK(worst <= 1.0e-5);
            continue;
        }
        const double worst = worstAbsolute(expected, actual);
        if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
            std::fprintf(stderr, "lens ramp %s: %.3g px\n", c.label, worst);
        }
        CAPTURE(c.label, compareFloat(expected, actual));
        CHECK(worst <= positionTolerance);
    }
}

TEST_CASE("A lens pass with nothing to do is no pass", "[gpu][lens]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = scene({61, 41});
    const DevelopState state;
    const std::size_t before = context.renderCount();
    const RenderCheckpoint lensed =
        developOnGpu(context, source, state, LensCorrection{}, Stage::Lens);
    CHECK(context.renderCount() == before);
    CHECK(sameBits(lensed.readBack(), source));

    // A table of ones is an identity the pass reproduces bit for bit.
    LensCorrection ones;
    ones.aspect = 61.0 / 41.0;
    ones.distortion.assign(lensTableNodes, 1.0F);
    ones.redScale.assign(lensTableNodes, 1.0F);
    ones.blueScale.assign(lensTableNodes, 1.0F);
    ones.vignetting.assign(lensTableNodes, 1.0F);
    const RenderCheckpoint same = developOnGpu(context, source, state, ones, Stage::Lens);
    CHECK(context.renderCount() == before + 1);
    CHECK(sameBits(same.readBack(), source));
}

TEST_CASE("A develop with lens corrections matches the CPU end to end", "[gpu][lens][develop]") {
    const ImageSize size{97, 61};
    // A smooth, positive picture: the chain's own error on edges and negatives is not the lens's.
    const ImageBuffer source = rainbow(size, PixelFormat::RgbaF32, workingEncoding);
    const LensCorrection lens = lensOf({.distortion = -0.06, .lateralCa = 0.003, .vignetting = 0.5},
                                       static_cast<double>(size.width) / size.height);
    DevelopState state;
    state.settings.tone.exposure = 0.4F;
    state.settings.geometry.straighten = 2.0;
    state.settings.effects.vignette.amount = -30.0F;
    RenderRequest request;
    request.size = RenderRequest::FitInside{70, 70};
    const ImageBuffer expected = develop(source, state, lens, request);
    const ImageBuffer actual =
        developOnGpu(gpuContext(), source, state, lens, Stage::Effects, request).readBack();
    REQUIRE(actual.size() == expected.size());
    const double colour = worstColourError(expected, actual);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "lens develop: colour %.3g\n", colour);
    }
    CAPTURE(colour);
    REQUIRE(colour <= lensDevelopTolerance);
    // And the corrections change the picture, so the comparison is not of two plain renders.
    REQUIRE(worstColourError(develop(source, state, LensCorrection{}, request), actual) > 1.0e-2);
}

TEST_CASE("A lens boundary resumes on the device", "[gpu][lens][resume]") {
    GpuContext& context = gpuContext();
    const ImageSize size{97, 61};
    const ImageBuffer source = scene(size);
    const LensCorrection lens = lensOf({.distortion = -0.06, .vignetting = 0.5},
                                       static_cast<double>(size.width) / size.height);
    DevelopState state;
    state.settings.noiseReduction.color = 40.0F;
    state.settings.tone.exposure = 0.3F;
    const RenderCheckpoint whole = developOnGpu(context, source, state, lens, Stage::Effects);
    const RenderCheckpoint atLens = developOnGpu(context, source, state, lens, Stage::Lens);
    REQUIRE(atLens.isResident());
    const RenderCheckpoint resumed =
        developOnGpu(context, atLens, source, state, lens, Stage::Effects);
    CHECK(sameBits(resumed.readBack(), whole.readBack()));
    // The same boundary with another lens is stale, and is refused.
    const LensCorrection other = lensOf({.distortion = -0.03}, lens.aspect);
    CHECK_THROWS_AS(developOnGpu(context, atLens, source, state, other, Stage::Effects),
                    std::invalid_argument);
}

TEST_CASE("A GPU ladder keeps a resident Lens rung, and resumes from it", "[gpu][lens][ladder]") {
    GpuContext& context = gpuContext();
    const ImageSize size{160, 100};
    const auto source = std::make_shared<const ImageBuffer>(scene(size));
    const DeviceImage uploaded = uploadSource(context, *source);
    const LensCorrection lens = lensOf({.distortion = -0.06, .lateralCa = 0.003, .vignetting = 0.5},
                                       static_cast<double>(size.width) / size.height);
    const RenderRequest request = fitting(80);
    const DevelopState state = stateWith(0.0F, 0.0, 0.0F, 40.0F);
    const auto expected = [&](const DevelopState& edited, const LensCorrection& corrected) {
        return developOnGpu(context, *source, uploaded, edited, corrected, Stage::Effects, request)
            .readBack();
    };

    CheckpointLadder ladder;
    const LadderRender fresh =
        resumeOrDevelopOnGpu(context, ladder, source, uploaded, state, lens, request);
    REQUIRE_FALSE(fresh.resumedFrom.has_value());
    CHECK(sameBits(fresh.checkpoint.readBack(), expected(state, lens)));
    CHECK(ladder.holds(Stage::Denoise));
    CHECK(ladder.holds(Stage::Lens));
    CHECK(ladder.holds(Stage::Pointwise));

    // An edit of the chain resumes from the Lens rung, which only a resident rung can serve.
    const DevelopState brighter = stateWith(0.5F, 0.0, 0.0F, 40.0F);
    const LadderRender edited =
        resumeOrDevelopOnGpu(context, ladder, source, uploaded, brighter, lens, request);
    REQUIRE(edited.resumedFrom == Stage::Lens);
    CHECK(sameBits(edited.checkpoint.readBack(), expected(brighter, lens)));

    // A lens toggle resumes from the Denoise rung, and runs the Lens pass again.
    const LadderRender off = resumeOrDevelopOnGpu(context, ladder, source, uploaded, brighter,
                                                  LensCorrection{}, request);
    REQUIRE(off.resumedFrom == Stage::Denoise);
    CHECK(sameBits(off.checkpoint.readBack(), expected(brighter, LensCorrection{})));
    CHECK_FALSE(ladder.holds(Stage::Lens));
    const LadderRender on =
        resumeOrDevelopOnGpu(context, ladder, source, uploaded, brighter, lens, request);
    REQUIRE(on.resumedFrom == Stage::Denoise);
    CHECK(sameBits(on.checkpoint.readBack(), expected(brighter, lens)));
    CHECK(ladder.holds(Stage::Lens));

    // Without noise reduction the Lens rung is the first, and a toggle renders from the source.
    CheckpointLadder plain;
    static_cast<void>(
        resumeOrDevelopOnGpu(context, plain, source, uploaded, DevelopState{}, lens, request));
    CHECK_FALSE(plain.holds(Stage::Denoise));
    CHECK(plain.holds(Stage::Lens));
    const LadderRender exposed =
        resumeOrDevelopOnGpu(context, plain, source, uploaded, stateWith(0.5F, 0.0), lens, request);
    CHECK(exposed.resumedFrom == Stage::Lens);
    const LadderRender removed = resumeOrDevelopOnGpu(
        context, plain, source, uploaded, stateWith(0.5F, 0.0), LensCorrection{}, request);
    CHECK_FALSE(removed.resumedFrom.has_value());
}

TEST_CASE("The lens pass reports its step, and a cancelled render stops before it",
          "[gpu][lens][progress][cancel]") {
    GpuContext& context = gpuContext();
    const ImageSize size{160, 100};
    const ImageBuffer source = scene(size);
    const LensCorrection lens = lensOf({.distortion = -0.06, .lateralCa = 0.003, .vignetting = 0.5},
                                       static_cast<double>(size.width) / size.height);
    DevelopState state;
    state.settings.noiseReduction.color = 40.0F;
    state.settings.effects.vignette.amount = -30.0F;

    std::vector<Progress> reports;
    ProgressChannel observing([&](const Progress& progress) { reports.push_back(progress); });
    const ImageBuffer observed =
        developOnGpu(context, source, state, lens, Stage::Effects, {}, &observing).readBack();
    std::vector<ProgressStep> steps;
    for (const Progress& report : reports) {
        if (steps.empty() || steps.back() != report.step) {
            steps.push_back(report.step);
        }
    }
    const auto position = [&](ProgressStep step) {
        return std::ranges::find(steps, step) - steps.begin();
    };
    REQUIRE(std::ranges::count(steps, ProgressStep::Lens) == 1);
    CHECK(position(ProgressStep::Denoise) < position(ProgressStep::Lens));
    CHECK(position(ProgressStep::Lens) < position(ProgressStep::Pointwise));
    CHECK(position(ProgressStep::Pointwise) < position(ProgressStep::Effects));
    REQUIRE_FALSE(reports.empty());
    CHECK(reports.back().fraction == 1.0);
    CHECK(
        sameBits(observed, developOnGpu(context, source, state, lens, Stage::Effects).readBack()));

    SECTION("before it starts, nothing is rendered") {
        ProgressChannel channel;
        channel.cancel();
        const std::size_t before = context.renderCount();
        CHECK_THROWS_AS(developOnGpu(context, source, state, lens, Stage::Effects, {}, &channel),
                        Cancelled);
        CHECK(context.renderCount() == before);
    }

    SECTION("when cancelled as the lens pass begins, it renders nothing") {
        std::size_t atCancel = 0;
        ProgressChannel* self = nullptr;
        ProgressChannel channel([&](const Progress& progress) {
            if (progress.step == ProgressStep::Lens && atCancel == 0) {
                atCancel = context.renderCount();
                self->cancel();
            }
        });
        self = &channel;
        CHECK_THROWS_AS(developOnGpu(context, source, state, lens, Stage::Effects, {}, &channel),
                        Cancelled);
        REQUIRE(atCancel != 0);
        CHECK(context.renderCount() == atCancel);
    }
}
