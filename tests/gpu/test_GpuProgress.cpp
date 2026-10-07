#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuTesting.h"
#include "ProgressScope.h"
#include "support/TestImages.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <Progress.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// The GPU's progress, a render at a time, and its cancellation between renders (ADR 042).

namespace {

/// @brief Settings that run every step a render has, and many renders in some.
DevelopState everyStep() {
    DevelopState state;
    state.settings.noiseReduction.luminance = 60.0F;
    state.settings.noiseReduction.color = 40.0F;
    state.settings.presence.clarity = 30.0F;
    state.settings.presence.texture = 20.0F;
    state.settings.presence.dehaze = 25.0F;
    state.settings.geometry.straighten = 3.0;
    state.settings.geometry.crop.rectangle = UprightCropRect{0.05, 0.1, 0.9, 0.95};
    state.settings.effects.vignette.amount = -30.0F;
    state.settings.effects.grain.amount = 25.0F;
    return state;
}

RenderRequest resized() {
    RenderRequest request;
    request.size = RenderRequest::FitInside{150, 150};
    return request;
}

ImageBuffer makeSource() {
    return rainbow({320, 200}, PixelFormat::RgbaF32, workingEncoding);
}

/// @brief Requires a stream of reports to be monotone, in step order, and to end at one.
void requireWellFormed(const std::vector<Progress>& reports) {
    REQUIRE_FALSE(reports.empty());
    for (std::size_t index = 1; index < reports.size(); ++index) {
        INFO("report " << index);
        REQUIRE(reports[index].fraction >= reports[index - 1].fraction);
        REQUIRE(static_cast<int>(reports[index].step) >= static_cast<int>(reports[index - 1].step));
    }
    REQUIRE(reports.back().fraction == 1.0);
}

} // namespace

TEST_CASE("A GPU render reports every step in order, and its bits do not change",
          "[gpu][progress]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = makeSource();
    const DevelopState state = everyStep();
    const ImageBuffer unobserved =
        developOnGpu(context, source, state, Stage::Effects, resized()).readBack();

    std::vector<Progress> reports;
    ProgressChannel channel([&](const Progress& progress) { reports.push_back(progress); });
    const ImageBuffer observed =
        developOnGpu(context, source, state, Stage::Effects, resized(), &channel).readBack();
    requireWellFormed(reports);
    std::vector<ProgressStep> steps;
    for (const Progress& report : reports) {
        if (steps.empty() || steps.back() != report.step) {
            steps.push_back(report.step);
        }
    }
    REQUIRE(steps == std::vector{ProgressStep::Denoise, ProgressStep::Context,
                                 ProgressStep::Pointwise, ProgressStep::Geometry,
                                 ProgressStep::Resize, ProgressStep::Effects});
    REQUIRE(compareFloat(unobserved, observed).bitExact);
}

TEST_CASE("A GPU render stops between renders when cancelled", "[gpu][progress][cancel]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = makeSource();
    const DevelopState state = everyStep();

    SECTION("before it starts, nothing is rendered") {
        ProgressChannel channel;
        channel.cancel();
        const std::size_t before = context.renderCount();
        REQUIRE_THROWS_AS(developOnGpu(context, source, state, Stage::Effects, resized(), &channel),
                          Cancelled);
        REQUIRE(context.renderCount() == before);
    }

    SECTION("part-way, no render follows the cancellation") {
        const std::size_t before = context.renderCount();
        static_cast<void>(developOnGpu(context, source, state, Stage::Effects, resized()));
        const std::size_t whole = context.renderCount() - before;

        std::size_t atCancel = 0;
        ProgressChannel* self = nullptr;
        ProgressChannel channel([&](const Progress& progress) {
            if (progress.step == ProgressStep::Context && atCancel == 0) {
                atCancel = context.renderCount();
                self->cancel();
            }
        });
        self = &channel;
        REQUIRE_THROWS_AS(developOnGpu(context, source, state, Stage::Effects, resized(), &channel),
                          Cancelled);
        REQUIRE(atCancel != 0);
        REQUIRE(context.renderCount() == atCancel);
        REQUIRE(context.renderCount() - before - whole < whole);
        REQUIRE(detail::currentProgress() == nullptr);
    }
}

TEST_CASE("A GPU chain of resumes reads as one render", "[gpu][progress][resume]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = makeSource();
    const DevelopState state = everyStep();
    std::vector<Progress> reports;
    ProgressChannel channel([&](const Progress& progress) { reports.push_back(progress); });
    RenderCheckpoint at = developOnGpu(context, source, state, Stage::Denoise, resized(), &channel);
    for (const Stage next : {Stage::Pointwise, Stage::Geometry, Stage::Resize, Stage::Effects}) {
        const std::size_t start = reports.size();
        REQUIRE(reports.back().fraction < 1.0);
        at = developOnGpu(context, at, source, state, next, resized(), &channel);
        REQUIRE(reports.size() > start);
        REQUIRE(reports[start].fraction == reports[start - 1].fraction);
    }
    requireWellFormed(reports);
}

TEST_CASE("A GPU sample reports and can be cancelled", "[gpu][progress][sample]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = makeSource();
    std::vector<Progress> reports;
    ProgressChannel channel([&](const Progress& progress) { reports.push_back(progress); });
    static_cast<void>(
        sampleOnGpu(context, source, everyStep(), Tap::CurveInput, resized(), &channel));
    requireWellFormed(reports);
    channel.cancel();
    REQUIRE_THROWS_AS(
        sampleOnGpu(context, source, everyStep(), Tap::CurveInput, resized(), &channel), Cancelled);
}

TEST_CASE("Each pass of a GPU render runs the renders its span declares", "[gpu][progress]") {
    // A debug build asserts that a span ran exactly the units it declared; each
    // case here runs a different set of renders.
    GpuContext& context = gpuContext();
    ImageBuffer translucent = makeSource();
    const auto samples = translucent.samples<float>();
    for (std::size_t index = 3; index < samples.size(); index += 8) {
        samples[index] = 0.5F;
    }
    const auto run = [&](const ImageBuffer& source, const DevelopState& state) {
        std::vector<Progress> reports;
        ProgressChannel channel([&](const Progress& progress) { reports.push_back(progress); });
        static_cast<void>(
            developOnGpu(context, source, state, Stage::Effects, resized(), &channel));
        requireWellFormed(reports);
    };
    DevelopState state;
    state.settings.noiseReduction.color = 40.0F;
    run(makeSource(), state);
    state = {};
    state.settings.noiseReduction.luminance = 40.0F;
    run(makeSource(), state);
    state = {};
    state.settings.presence.texture = 40.0F;
    run(makeSource(), state);
    state = {};
    state.settings.presence.dehaze = -40.0F;
    run(makeSource(), state);
    state = {};
    state.settings.presence.clarity = 40.0F;
    run(translucent, state);
    run(translucent, everyStep());
}
