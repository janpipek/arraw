#include "support/LadderTesting.h"
#include "support/RenderDigest.h"
#include "support/TestImages.h"

#include <CheckpointLadder.h>
#include <Develop.h>
#include <DevelopState.h>
#include <NoiseReductionSettings.h>
#include <Progress.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// The checkpoint ladder on the host: what a render through it keeps, resumes from and drops.

namespace {

/// @brief Makes the source the sequence tests render.
std::shared_ptr<const ImageBuffer> makeSource() {
    return std::make_shared<const ImageBuffer>(
        rainbow({160, 100}, PixelFormat::RgbaF32, workingEncoding));
}

/// @brief Edits a state in one stage, so that only that stage and the later ones change.
struct Edit {
    const char* label;                              ///< Name for a failure.
    void (*apply)(DevelopState&);                   ///< Edits the state.
    RenderRequest (*request)(const RenderRequest&); ///< Edits the request.
    std::optional<Stage> (*expected)(
        const DevelopState&); ///< Boundary a ladder at the state resumes from.
};

/// @brief The edits, one for each stage a render can resume after.
const std::vector<Edit>& edits() {
    static const std::vector<Edit> all{
        {"tone", [](DevelopState& state) { state.settings.tone.exposure += 0.1F; },
         [](const RenderRequest& request) { return request; },
         [](const DevelopState& state) -> std::optional<Stage> {
             // The denoise result does not depend on tone, but only exists with noise reduction on.
             return reducesNoise(state.settings.noiseReduction) ? std::optional(Stage::Denoise)
                                                                : std::nullopt;
         }},
        {"geometry", [](DevelopState& state) { state.settings.geometry.straighten += 1.0; },
         [](const RenderRequest& request) { return request; },
         [](const DevelopState&) -> std::optional<Stage> { return Stage::Pointwise; }},
        {"size", [](DevelopState&) {},
         [](const RenderRequest& request) {
             RenderRequest changed = request;
             changed.size = RenderRequest::FitInside{15, 15};
             changed.region.reset();
             return changed;
         },
         [](const DevelopState&) -> std::optional<Stage> { return Stage::Geometry; }},
        {"vignette", [](DevelopState& state) { state.settings.effects.vignette.amount = -33.0F; },
         [](const RenderRequest& request) { return request; },
         [](const DevelopState&) -> std::optional<Stage> { return Stage::Resize; }},
    };
    return all;
}

} // namespace

TEST_CASE("A ladder render equals develop, fresh and resumed after each kind of edit",
          "[ladder][develop][slow]") {
    const auto sources = digestSources();
    const auto states = digestStates();
    const auto requests = digestRequests();
    for (const auto& named : sources) {
        // Only the sources of the matrix that differ in what they exercise.
        if (named.name != "bayer-32x24.dng" && named.name != "rainbow-97x61") {
            continue;
        }
        const auto source = std::make_shared<const ImageBuffer>(named.buffer.clone());
        for (const auto& state : states) {
            for (const auto& request : requests) {
                INFO(named.name << " / " << state.name << " / " << request.name);
                CheckpointLadder ladder;
                const LadderRender fresh =
                    resumeOrDevelop(ladder, source, state.state, request.request);
                REQUIRE_FALSE(fresh.resumedFrom.has_value());
                REQUIRE(sameBits(fresh.checkpoint.readBack(),
                                 develop(*source, state.state, request.request)));
                for (const Edit& edit : edits()) {
                    INFO("then edited in " << edit.label);
                    // Every edit starts from the ladder the fresh render left.
                    CheckpointLadder resumed;
                    static_cast<void>(
                        resumeOrDevelop(resumed, source, state.state, request.request));
                    DevelopState edited = state.state;
                    edit.apply(edited);
                    const RenderRequest asked = edit.request(request.request);
                    const LadderRender render = resumeOrDevelop(resumed, source, edited, asked);
                    REQUIRE(render.resumedFrom == edit.expected(state.state));
                    REQUIRE(
                        sameBits(render.checkpoint.readBack(), develop(*source, edited, asked)));
                }
            }
        }
    }
}

TEST_CASE("After a fresh render a ladder holds the boundaries passed, and never the effects",
          "[ladder]") {
    const auto source = makeSource();
    CheckpointLadder ladder;
    REQUIRE(ladder.empty());

    static_cast<void>(resumeOrDevelop(ladder, source, stateWith(0.0F, 0.0, -40.0F), fitting(80)));
    REQUIRE_FALSE(ladder.empty());
    // A collapsed geometry is kept: a viewport change resumes from it.
    REQUIRE(ladder.holds(Stage::Pointwise));
    REQUIRE(ladder.holds(Stage::Geometry));
    REQUIRE(ladder.holds(Stage::Resize));
    REQUIRE_FALSE(ladder.holds(Stage::Denoise));
    REQUIRE_FALSE(ladder.holds(Stage::Effects));

    CheckpointLadder denoising;
    static_cast<void>(
        resumeOrDevelop(denoising, source, stateWith(0.0F, 3.0, -40.0F, 40.0F), fitting(80)));
    for (const Stage stage : {Stage::Denoise, Stage::Pointwise, Stage::Geometry, Stage::Resize}) {
        REQUIRE(denoising.holds(stage));
    }
    REQUIRE_FALSE(denoising.holds(Stage::Effects));
}

TEST_CASE("An edit resumes from the newest rung it can still use, and drops the ones it cannot",
          "[ladder][resume]") {
    const auto source = makeSource();
    const RenderRequest wide = fitting(100);
    const RenderRequest narrow = fitting(70);
    struct Step {
        const char* label;
        DevelopState state;
        RenderRequest request;
        std::optional<Stage> resumed;
    };
    // The sequence of the preview's test of the same name, at the engine.
    const std::vector<Step> steps{
        {"the first render develops from the source", stateWith(0.0F, 0.0), wide, std::nullopt},
        {"a tone change resumes from nothing", stateWith(0.5F, 0.0), wide, std::nullopt},
        {"a viewport change resumes from the geometry", stateWith(0.5F, 0.0), narrow,
         Stage::Geometry},
        {"a straighten change resumes from the pointwise result", stateWith(0.5F, 5.0), narrow,
         Stage::Pointwise},
        {"the same request again resumes from the resize", stateWith(0.5F, 5.0), narrow,
         Stage::Resize},
        {"a tone change after that resumes from nothing", stateWith(-0.5F, 5.0), narrow,
         std::nullopt},
        {"a viewport change after that resumes from the geometry", stateWith(-0.5F, 5.0), wide,
         Stage::Geometry},
        {"a vignette change resumes from the resize", stateWith(-0.5F, 5.0, -40.0F), wide,
         Stage::Resize},
        {"a viewport change with a vignette resumes from the geometry",
         stateWith(-0.5F, 5.0, -40.0F), narrow, Stage::Geometry},
        {"a vignette turned off resumes from the resize", stateWith(-0.5F, 5.0), narrow,
         Stage::Resize},
        {"noise reduction turned on develops from the source", stateWith(-0.5F, 5.0, 0.0F, 40.0F),
         narrow, std::nullopt},
        {"a tone change with noise reduction resumes from the denoise result",
         stateWith(0.25F, 5.0, 0.0F, 40.0F), narrow, Stage::Denoise},
        {"a stronger noise reduction develops from the source again",
         stateWith(0.25F, 5.0, 0.0F, 70.0F), narrow, std::nullopt},
        {"noise reduction turned off develops from the source, keeping no denoise result",
         stateWith(0.25F, 5.0), narrow, std::nullopt},
        {"a tone change then resumes from nothing again", stateWith(0.5F, 5.0), narrow,
         std::nullopt},
    };
    CheckpointLadder ladder;
    for (const Step& step : steps) {
        INFO(step.label);
        const LadderRender render = resumeOrDevelop(ladder, source, step.state, step.request);
        REQUIRE(render.resumedFrom == step.resumed);
        REQUIRE(sameBits(render.checkpoint.readBack(), develop(*source, step.state, step.request)));
        // Whatever the render resumed from, the rungs it passed are held again.
        REQUIRE(ladder.holds(Stage::Pointwise));
        REQUIRE(ladder.holds(Stage::Geometry));
        REQUIRE(ladder.holds(Stage::Resize));
        REQUIRE(ladder.holds(Stage::Denoise) == reducesNoise(step.state.settings.noiseReduction));
    }
}

TEST_CASE("Another source clears a ladder, and so does clear", "[ladder]") {
    const auto source = makeSource();
    const DevelopState state = stateWith(0.3F, 2.0);
    CheckpointLadder ladder;
    static_cast<void>(resumeOrDevelop(ladder, source, state, fitting(80)));
    REQUIRE(resumeOrDevelop(ladder, source, state, fitting(80)).resumedFrom == Stage::Resize);

    // Same size, encoding and settings, other pixels: only the ladder's binding tells.
    ImageBuffer changed = source->clone();
    changed.samples<float>()[0] += 0.5F;
    const auto other = std::make_shared<const ImageBuffer>(std::move(changed));
    const LadderRender again = resumeOrDevelop(ladder, other, state, fitting(80));
    REQUIRE_FALSE(again.resumedFrom.has_value());
    REQUIRE(sameBits(again.checkpoint.readBack(), develop(*other, state, fitting(80))));

    ladder.clear();
    REQUIRE(ladder.empty());
    REQUIRE_FALSE(resumeOrDevelop(ladder, other, state, fitting(80)).resumedFrom.has_value());
}

TEST_CASE("A request that cannot be rendered throws and leaves the ladder as it was",
          "[ladder][region]") {
    const auto source = makeSource();
    const DevelopState state = stateWith(0.3F, 2.0);
    RenderRequest bad = fitting(80);
    bad.region = RenderRequest::Region{0.6, 0.1, 0.2, 0.9};

    CheckpointLadder empty;
    REQUIRE_THROWS_AS(resumeOrDevelop(empty, source, state, bad), std::invalid_argument);
    REQUIRE(empty.empty());

    CheckpointLadder ladder;
    static_cast<void>(resumeOrDevelop(ladder, source, state, fitting(80)));
    REQUIRE_THROWS_AS(resumeOrDevelop(ladder, source, state, bad), std::invalid_argument);
    REQUIRE(ladder.holds(Stage::Pointwise));
    REQUIRE(ladder.holds(Stage::Geometry));
    REQUIRE(ladder.holds(Stage::Resize));
    REQUIRE(resumeOrDevelop(ladder, source, state, fitting(80)).resumedFrom == Stage::Resize);

    REQUIRE_THROWS_AS(resumeOrDevelop(ladder, nullptr, state, fitting(80)), std::invalid_argument);
}

TEST_CASE("A render cancelled part-way keeps the rungs it finished, and the next resumes from them",
          "[ladder][progress][cancel]") {
    const auto source = makeSource();
    const DevelopState state = stateWith(0.3F, 3.0, -30.0F);
    CheckpointLadder ladder;
    ProgressChannel* self = nullptr;
    ProgressChannel channel([&](const Progress& progress) {
        if (progress.step == ProgressStep::Resize) {
            self->cancel();
        }
    });
    self = &channel;
    REQUIRE_THROWS_AS(resumeOrDevelop(ladder, source, state, fitting(60), &channel), Cancelled);
    REQUIRE(ladder.holds(Stage::Pointwise));
    REQUIRE(ladder.holds(Stage::Geometry));
    REQUIRE_FALSE(ladder.holds(Stage::Resize));

    const LadderRender next = resumeOrDevelop(ladder, source, state, fitting(60));
    REQUIRE(next.resumedFrom == Stage::Geometry);
    REQUIRE(sameBits(next.checkpoint.readBack(), develop(*source, state, fitting(60))));
}

TEST_CASE("An observed ladder render reports from the share of the rung it resumes from",
          "[ladder][progress]") {
    const auto source = makeSource();
    const DevelopState state = stateWith(0.3F, 3.0, -30.0F, 40.0F);
    CheckpointLadder ladder;

    ReportLog fresh;
    ProgressChannel freshChannel(fresh.callback());
    static_cast<void>(resumeOrDevelop(ladder, source, state, fitting(60), &freshChannel));
    REQUIRE(fresh.wellFormed());
    REQUIRE(fresh.reports.front().step == ProgressStep::Denoise);

    DevelopState edited = state;
    edited.settings.effects.vignette.amount = -10.0F;
    ReportLog resumed;
    ProgressChannel resumedChannel(resumed.callback());
    const LadderRender render =
        resumeOrDevelop(ladder, source, edited, fitting(60), &resumedChannel);
    REQUIRE(render.resumedFrom == Stage::Resize);
    REQUIRE(resumed.wellFormed());
    REQUIRE(resumed.reports.front().step == ProgressStep::Effects);
    REQUIRE(resumed.reports.front().fraction > 0.0);

    // Observing changes no bit.
    REQUIRE(sameBits(render.checkpoint.readBack(), develop(*source, edited, fitting(60))));
}
