#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuTesting.h"
#include "support/LadderTesting.h"
#include "support/RenderDigest.h"
#include "support/TestImages.h"

#include <CheckpointLadder.h>
#include <Develop.h>
#include <DevelopState.h>
#include <NoiseReductionSettings.h>
#include <Progress.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// The checkpoint ladder on a device: the same rules as on the host, with rungs resident.

namespace {

/// @brief Makes the source the tests render.
std::shared_ptr<const ImageBuffer> makeSource() {
    return std::make_shared<const ImageBuffer>(
        rainbow({160, 100}, PixelFormat::RgbaF32, workingEncoding));
}

} // namespace

TEST_CASE("A GPU ladder render equals developOnGpu, fresh and resumed after each kind of edit",
          "[gpu][ladder]") {
    GpuContext& context = gpuContext();
    const auto sources = digestSources();
    const auto states = digestStates();
    for (const auto& named : sources) {
        if (named.name != "bayer-32x24.dng" && named.name != "rainbow-97x61") {
            continue;
        }
        const auto source = std::make_shared<const ImageBuffer>(named.buffer.clone());
        const DeviceImage uploaded = uploadSource(context, *source);
        for (const auto& state : states) {
            for (const RenderRequest& request : {RenderRequest{}, fitting(40)}) {
                INFO(named.name << " / " << state.name);
                const auto expected = [&](const DevelopState& edited, const RenderRequest& asked) {
                    return developOnGpu(context, *source, uploaded, edited, Stage::Effects, asked)
                        .readBack();
                };
                CheckpointLadder ladder;
                const LadderRender fresh =
                    resumeOrDevelopOnGpu(context, ladder, source, uploaded, state.state, request);
                REQUIRE_FALSE(fresh.resumedFrom.has_value());
                REQUIRE(fresh.checkpoint.isResident());
                REQUIRE(sameBits(fresh.checkpoint.readBack(), expected(state.state, request)));

                struct Edit {
                    std::optional<Stage> resumed;
                    DevelopState state;
                    RenderRequest request;
                };
                DevelopState tone = state.state;
                tone.settings.tone.exposure += 0.1F;
                DevelopState geometry = state.state;
                geometry.settings.geometry.straighten += 1.0;
                DevelopState vignette = state.state;
                vignette.settings.effects.vignette.amount = -33.0F;
                const std::vector<Edit> edits{
                    {reducesNoise(state.state.settings.noiseReduction)
                         ? std::optional(Stage::Denoise)
                         : std::nullopt,
                     tone, request},
                    {Stage::Pointwise, geometry, request},
                    {Stage::Geometry, state.state, fitting(15)},
                    {Stage::Resize, vignette, request},
                };
                for (const Edit& edit : edits) {
                    CheckpointLadder resumed;
                    static_cast<void>(resumeOrDevelopOnGpu(context, resumed, source, uploaded,
                                                           state.state, request));
                    const LadderRender render = resumeOrDevelopOnGpu(
                        context, resumed, source, uploaded, edit.state, edit.request);
                    REQUIRE(render.resumedFrom == edit.resumed);
                    REQUIRE(
                        sameBits(render.checkpoint.readBack(), expected(edit.state, edit.request)));
                }
            }
        }
    }
}

TEST_CASE("A GPU ladder holds the boundaries passed, and never the effects", "[gpu][ladder]") {
    GpuContext& context = gpuContext();
    const auto source = makeSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    CheckpointLadder ladder;
    static_cast<void>(resumeOrDevelopOnGpu(context, ladder, source, uploaded,
                                           stateWith(0.0F, 0.0, -40.0F), fitting(80)));
    for (const Stage stage : {Stage::Pointwise, Stage::Geometry, Stage::Resize}) {
        REQUIRE(ladder.holds(stage));
    }
    REQUIRE_FALSE(ladder.holds(Stage::Denoise));
    REQUIRE_FALSE(ladder.holds(Stage::Effects));

    CheckpointLadder denoising;
    static_cast<void>(resumeOrDevelopOnGpu(context, denoising, source, uploaded,
                                           stateWith(0.0F, 3.0, 0.0F, 40.0F), fitting(80)));
    REQUIRE(denoising.holds(Stage::Denoise));

    denoising.clear();
    REQUIRE(denoising.empty());
}

TEST_CASE("A ladder render issues the renders of the passes it runs and no more", "[gpu][ladder]") {
    GpuContext& context = gpuContext();
    const auto source = makeSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    const DevelopState state = stateWith(0.3F, 3.0, -30.0F, 40.0F);
    const RenderRequest request = fitting(60);

    const auto rendersOf = [&](auto&& render) {
        const std::size_t before = context.renderCount();
        render();
        return context.renderCount() - before;
    };
    const std::size_t direct = rendersOf([&] {
        static_cast<void>(developOnGpu(context, *source, uploaded, state, Stage::Effects, request));
    });
    CheckpointLadder ladder;
    const std::size_t fresh = rendersOf([&] {
        static_cast<void>(resumeOrDevelopOnGpu(context, ladder, source, uploaded, state, request));
    });
    REQUIRE(fresh == direct);

    // With the effects off, the resize rung is the result: nothing is rendered.
    const DevelopState plain = stateWith(0.3F, 3.0, 0.0F, 40.0F);
    static_cast<void>(resumeOrDevelopOnGpu(context, ladder, source, uploaded, plain, request));
    std::optional<LadderRender> again;
    const std::size_t resumed = rendersOf([&] {
        again.emplace(resumeOrDevelopOnGpu(context, ladder, source, uploaded, plain, request));
    });
    REQUIRE(again->resumedFrom == Stage::Resize);
    REQUIRE(resumed == 0);
}

TEST_CASE("A ladder drops the rungs of the other backend, and renders from the source",
          "[gpu][ladder]") {
    GpuContext& context = gpuContext();
    const auto source = makeSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    const DevelopState state = stateWith(0.3F, 3.0, -30.0F);
    const RenderRequest request = fitting(60);

    // Host rungs, then the device entry.
    CheckpointLadder ladder;
    static_cast<void>(resumeOrDevelop(ladder, source, state, request));
    REQUIRE(ladder.holds(Stage::Resize));
    const LadderRender onDevice =
        resumeOrDevelopOnGpu(context, ladder, source, uploaded, state, request);
    REQUIRE_FALSE(onDevice.resumedFrom.has_value());
    REQUIRE(onDevice.checkpoint.isResident());
    REQUIRE(sameBits(
        onDevice.checkpoint.readBack(),
        developOnGpu(context, *source, uploaded, state, Stage::Effects, request).readBack()));

    // And back: the resident rungs are dropped by the host entry.
    const LadderRender onHost = resumeOrDevelop(ladder, source, state, request);
    REQUIRE_FALSE(onHost.resumedFrom.has_value());
    REQUIRE_FALSE(onHost.checkpoint.isResident());
    REQUIRE(sameBits(onHost.checkpoint.readBack(), develop(*source, state, request)));
}

TEST_CASE("A GPU ladder render refuses a bad request without touching the ladder, and a foreign "
          "upload",
          "[gpu][ladder]") {
    GpuContext& context = gpuContext();
    const auto source = makeSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    const DevelopState state = stateWith(0.3F, 2.0);
    CheckpointLadder ladder;
    static_cast<void>(resumeOrDevelopOnGpu(context, ladder, source, uploaded, state, fitting(80)));

    RenderRequest bad = fitting(80);
    bad.region = RenderRequest::Region{0.6, 0.1, 0.2, 0.9};
    REQUIRE_THROWS_AS(resumeOrDevelopOnGpu(context, ladder, source, uploaded, state, bad),
                      std::invalid_argument);
    REQUIRE(ladder.holds(Stage::Resize));
    REQUIRE_THROWS_AS(resumeOrDevelopOnGpu(context, ladder, nullptr, uploaded, state, fitting(80)),
                      std::invalid_argument);

    // An upload of another source (here of another size) is refused too.
    const auto other = std::make_shared<const ImageBuffer>(
        rainbow({64, 40}, PixelFormat::RgbaF32, workingEncoding));
    const DeviceImage foreign = uploadSource(context, *other);
    REQUIRE_THROWS_AS(resumeOrDevelopOnGpu(context, ladder, source, foreign, state, fitting(80)),
                      std::invalid_argument);
    REQUIRE(ladder.holds(Stage::Resize));
}

TEST_CASE("A GPU ladder render cancelled part-way keeps the rungs it finished, and reports from "
          "the rung it resumes",
          "[gpu][ladder][progress][cancel]") {
    GpuContext& context = gpuContext();
    const auto source = makeSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    const DevelopState state = stateWith(0.3F, 3.0, -30.0F);
    CheckpointLadder ladder;
    ProgressChannel* self = nullptr;
    ProgressChannel channel([&](const Progress& progress) {
        if (progress.step == ProgressStep::Resize) {
            self->cancel();
        }
    });
    self = &channel;
    REQUIRE_THROWS_AS(
        resumeOrDevelopOnGpu(context, ladder, source, uploaded, state, fitting(60), &channel),
        Cancelled);
    REQUIRE(ladder.holds(Stage::Pointwise));
    REQUIRE(ladder.holds(Stage::Geometry));
    REQUIRE_FALSE(ladder.holds(Stage::Resize));

    ReportLog log;
    ProgressChannel observed(log.callback());
    const LadderRender next =
        resumeOrDevelopOnGpu(context, ladder, source, uploaded, state, fitting(60), &observed);
    REQUIRE(next.resumedFrom == Stage::Geometry);
    REQUIRE(log.wellFormed());
    REQUIRE(log.reports.front().step == ProgressStep::Resize);
}
