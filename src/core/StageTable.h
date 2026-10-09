#pragma once

#include "ProcessingPlan.h"

#include <Progress.h>
#include <RenderCheckpoint.h>

#include <array>
#include <cstddef>

namespace arraw {

/// @brief Row of the stage table: what one pass boundary is, whichever backend runs it.
///
/// The backend-independent facts of a stage. The plan block a stage reads is
/// ::arraw::stagesOf, which already maps each stage to its block; what each
/// backend calls to run the pass is its own `switch` over ::arraw::Stage,
/// since core does not link the GPU (ADR 045).
struct StageRow {
    /// Boundary the row ends at.
    Stage stage;
    /// First progress step the pass runs.
    ProgressStep firstStep;
    /// Last progress step the pass runs.
    ProgressStep lastStep;
    /// Whether the pass runs; otherwise its boundary collapses onto the one before.
    bool (*runs)(const ProcessingPlan& plan);
    /// Size of the pixels at the boundary.
    ImageSize (*sizeAt)(const ProcessingPlan& plan, ImageSize source);
};

/// @brief The stage table, one row per ::arraw::Stage in pipeline order.
///
/// The `runs` predicates read the geometry and the resize, which a plan from
/// pixels always has; the driver only asks about a stage the render reaches.
inline constexpr std::array<StageRow, stageCount> stageTable{{
    {Stage::Denoise, ProgressStep::Denoise, ProgressStep::Denoise,
     [](const ProcessingPlan& plan) { return plan.denoise.active(); },
     [](const ProcessingPlan&, ImageSize source) { return source; }},
    {Stage::Pointwise, ProgressStep::Context, ProgressStep::Pointwise,
     [](const ProcessingPlan&) { return true; },
     [](const ProcessingPlan&, ImageSize source) { return source; }},
    {Stage::Geometry, ProgressStep::Geometry, ProgressStep::Geometry,
     [](const ProcessingPlan& plan) { return !plan.geometry->isIdentity(); },
     [](const ProcessingPlan& plan, ImageSize) { return plan.geometry->outputSize; }},
    {Stage::Resize, ProgressStep::Resize, ProgressStep::Resize,
     [](const ProcessingPlan& plan) { return !plan.resize->isIdentity(plan.geometry->outputSize); },
     [](const ProcessingPlan& plan, ImageSize) { return plan.resize->outputSize; }},
    {Stage::Effects, ProgressStep::Effects, ProgressStep::Effects,
     [](const ProcessingPlan& plan) { return plan.effects.active(); },
     [](const ProcessingPlan& plan, ImageSize) { return plan.resize->outputSize; }},
}};

namespace detail {

/// @brief Whether every row of the table sits at the index of its stage.
[[nodiscard]] constexpr bool stageTableInOrder() noexcept {
    for (std::size_t index = 0; index < stageCount; ++index) {
        if (static_cast<std::size_t>(stageTable[index].stage) != index) {
            return false;
        }
    }
    return true;
}

} // namespace detail

static_assert(detail::stageTableInOrder(), "row i of the stage table must be Stage i");

/// @brief Gives the row of a stage.
/// @param stage A known stage.
[[nodiscard]] constexpr const StageRow& rowOf(Stage stage) noexcept {
    return stageTable[static_cast<std::size_t>(stage)];
}

/// @brief Gives the stage that follows another.
/// @param stage A known stage other than ::arraw::Stage::Effects.
[[nodiscard]] constexpr Stage following(Stage stage) noexcept {
    return static_cast<Stage>(static_cast<std::size_t>(stage) + 1);
}

/// @brief Whether a render that passes a boundary before its last keeps a rung there.
///
/// Every boundary but the last, except a collapsed Denoise, which would only
/// copy the source the caller keeps anyway. A collapsed Geometry is kept: the
/// preview resumes from it for a viewport change whether or not a geometry is
/// set. The last boundary, Effects, is the render's result and is never kept.
/// @param boundary Boundary just passed.
/// @param plan The render's plan.
[[nodiscard]] inline bool keepsRung(Stage boundary, const ProcessingPlan& plan) {
    switch (boundary) {
    case Stage::Denoise:
        return rowOf(Stage::Denoise).runs(plan);
    case Stage::Pointwise:
    case Stage::Geometry:
    case Stage::Resize:
        return true;
    case Stage::Effects:
        break;
    }
    return false;
}

} // namespace arraw
