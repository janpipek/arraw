#pragma once

#include "ProcessingPlan.h"
#include "ProgressScope.h"

#include <Develop.h>
#include <Progress.h>
#include <RenderCheckpoint.h>

#include <array>

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
[[nodiscard]] StepWeights renderStepWeights(const ProcessingPlan& plan,
                                            const RenderRequest& request) noexcept;

/// @brief Gives the step weights of a render that may be observed: none when it is not.
///
/// So that an unobserved render does not pay for working them out.
/// @param progress The caller's channel, or null.
/// @param plan The render's plan.
/// @param request What the whole render is asked for.
[[nodiscard]] inline StepWeights observedStepWeights(const ProgressChannel* progress,
                                                     const ProcessingPlan& plan,
                                                     const RenderRequest& request) noexcept {
    return progress != nullptr ? renderStepWeights(plan, request) : StepWeights{};
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
