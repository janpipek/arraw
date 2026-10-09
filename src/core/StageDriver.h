#pragma once

#include "LadderAccess.h"
#include "ProcessingPlan.h"
#include "StageTable.h"

#include <CheckpointLadder.h>
#include <RenderCheckpoint.h>

#include <concepts>
#include <optional>
#include <utility>

namespace arraw {

/// @brief What a backend gives the stage driver: its pixels, and the three things it does to them.
///
/// The two backends cannot share a pixel type: an ::arraw::ImageBuffer is
/// move-only and may be borrowed, a device image is a shared handle. The
/// driver is therefore a template over the backend, whose `Pixels` are its own,
/// and whose three functions are all that differs:
/// - `run` makes the pixels at a stage's boundary from those at the one before;
///   the driver has already checked that the stage's row says it runs.
/// - `checkpoint` turns the pixels at a boundary into a checkpoint, consuming them.
/// - `borrow` gives pixels that read a checkpoint's, to carry on from it.
template <typename B>
concept StageBackend = requires(B& backend, typename B::Pixels pixels, const ProcessingPlan& plan,
                                Stage stage, const RenderCheckpoint& checkpoint) {
    { backend.run(stage, std::move(pixels), plan) } -> std::same_as<typename B::Pixels>;
    { backend.checkpoint(stage, std::move(pixels), plan) } -> std::same_as<RenderCheckpoint>;
    { backend.borrow(checkpoint) } -> std::same_as<typename B::Pixels>;
};

/// @brief The outcome of ::arraw::runStages.
template <StageBackend B> struct StagesRun {
    /// Pixels at the last boundary run.
    typename B::Pixels pixels;
    /// That boundary.
    Stage done;
};

/// @brief Runs the passes after a boundary up to another, skipping those that collapse.
///
/// The one loop of both backends (ADR 045): a pass whose row in the stage table
/// says it does not run leaves the pixels as they are, and its boundary is
/// still passed. With a ladder, each boundary before the last that
/// ::arraw::keepsRung names is stored as a rung as soon as its pass returned,
/// and the run carries on from the rung's pixels, so a cancelled or failed
/// render leaves the rungs it finished and no partial one.
/// @param backend What runs the passes.
/// @param done Boundary @p pixels were taken at, or empty for the source.
/// @param pixels The source, or the pixels at @p done.
/// @param plan The render's plan.
/// @param stopAfter Last boundary to run, not before @p done.
/// @param ladder Where to keep the rungs passed, or null for none.
template <StageBackend B>
[[nodiscard]] StagesRun<B> runStages(B& backend, std::optional<Stage> done,
                                     typename B::Pixels pixels, const ProcessingPlan& plan,
                                     Stage stopAfter, CheckpointLadder* ladder = nullptr) {
    std::optional<Stage> at = done;
    while (!at || *at < stopAfter) {
        const Stage next = at ? following(*at) : Stage::Denoise;
        if (rowOf(next).runs(plan)) {
            pixels = backend.run(next, std::move(pixels), plan);
        }
        at = next;
        if (ladder != nullptr && next != stopAfter && keepsRung(next, plan)) {
            const RenderCheckpoint rung = backend.checkpoint(next, std::move(pixels), plan);
            LadderAccess::store(*ladder, rung);
            pixels = backend.borrow(rung);
        }
    }
    return {std::move(pixels), *at};
}

} // namespace arraw
