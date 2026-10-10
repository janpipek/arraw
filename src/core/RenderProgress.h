#pragma once

#include "BrushCoverage.h"
#include "ProcessingPlan.h"
#include "ProgressScope.h"

#include <Develop.h>
#include <Progress.h>
#include <RenderCheckpoint.h>

#include <array>
#include <vector>

namespace arraw::detail {

/// @brief Gives the relative cost of each step of a whole render, from the source to the effects.
///
/// Wall time on the CPU as measured on a release build (ADR 042), from the
/// pixels each step covers: zero for a step the plan does not run. The same
/// for every call of a chain of resumes, which is what lets the chain read as
/// one render: the resize and the effects are sized from @p request rather
/// than from the plan, whose resize is the identity when a call stops before
/// it. The GPU's steps take the same shares, which are the CPU's: the ratios
/// between its passes are close enough, and a GPU render is rarely long
/// enough to be shown at all.
/// @param plan The render's plan.
/// @param request What the whole render is asked for; one that cannot be
/// resolved is taken as no resize.
/// @param residency The ladder's packed coverage when the render goes through one, else null: the
/// Coverage step counts only the brushes whose coverage is not ready (see
/// ::arraw::detail::coverageUnitWeights).
[[nodiscard]] StepWeights renderStepWeights(const ProcessingPlan& plan,
                                            const RenderRequest& request,
                                            const CoverageResidency* residency = nullptr) noexcept;

/// @brief What one brush's coverage costs a render, in nanoseconds of the time model.
struct CoverageWork {
    double draw = 0.0; ///< Drawing the strokes not yet in the cache.
    double pack = 0.0; ///< Quantising into the packed planes.
};

/// @brief Gives the cost of a brush's coverage for a status.
///
/// Nothing for a held brush. Packing is `packCost` per pixel; drawing is `coverageCost` per unit
/// of swept area times the long edge squared (report B7), of the strokes the cache does not hold:
/// all for a brush drawn from nothing, those after the prefix for an extended one, none for a
/// cached one.
[[nodiscard]] CoverageWork coverageWorkOf(const CoverageStatus& status,
                                          const BrushCoverageRef& brush) noexcept;

/// @brief Gives the cost of each brush whose coverage a render still has to work on.
///
/// A brush the residency holds costs nothing and is left out. One the cache holds, whole or as an
/// equal list, costs its packing (`packCost` per pixel); any other costs also its drawing
/// (`coverageCost` per unit of swept area times the long edge squared, the time model of report
/// B7). Draws, inserts and packs nothing.
/// @param local The plan's local block.
/// @param residency The ladder's packed coverage, or null for a direct render.
/// @return One weight per brush to work on, in plan order; their sum is the Coverage step's cost.
[[nodiscard]] std::vector<double> coverageUnitWeights(const LocalPlan& local,
                                                      const CoverageResidency* residency);

/// @brief Gives the step weights of a render that may be observed: none when it is not.
///
/// So that an unobserved render does not pay for working them out.
/// @param progress The caller's channel, or null.
/// @param plan The render's plan.
/// @param request What the whole render is asked for.
/// @param residency The ladder's packed coverage when the render goes through one, else null.
[[nodiscard]] inline StepWeights
observedStepWeights(const ProgressChannel* progress, const ProcessingPlan& plan,
                    const RenderRequest& request,
                    const CoverageResidency* residency = nullptr) noexcept {
    return progress != nullptr ? renderStepWeights(plan, request, residency) : StepWeights{};
}

/// @brief Gives the first step a render resuming from a boundary runs.
/// @param boundary Boundary the render resumes from.
[[nodiscard]] ProgressStep stepAfter(Stage boundary) noexcept;

/// @brief Gives the last step a render stopping after a boundary runs.
/// @param boundary Boundary the render stops after.
[[nodiscard]] ProgressStep stepThrough(Stage boundary) noexcept;

/// @brief Gives the relative cost of the two passes of a resize from one size to another.
///
/// What the resize's span splits its share by, and what
/// ::arraw::detail::renderStepWeights counts for the step.
/// @param input Size of the region resized.
/// @param output Size of the result.
/// @return The costs of the horizontal pass and of the vertical one, which
/// reads the horizontal one's rows of the result's width.
[[nodiscard]] std::array<double, 2> resizeUnitWeights(ImageSize input, ImageSize output) noexcept;

} // namespace arraw::detail
