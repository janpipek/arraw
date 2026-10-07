#pragma once

#include "ProcessingPlan.h"

#include <ImageBuffer.h>

#include <memory>
#include <optional>

namespace arraw::app {

/// @brief Record of what the last curve histogram counted, telling when to count again.
///
/// A curve histogram costs a render (ADR 035), so the preview counts again only
/// when the curve input could have changed: a new source or pyramid level, or a
/// plan that ::arraw::sameAtTap says differs at ::arraw::Tap::CurveInput. It
/// never keeps a list of its own of the settings before the curves, which would
/// drift from the chain: dragging a curve, or any control after the tap, keeps
/// the histogram current (ADR 036).
class CurveHistogramRefresh {
public:
    /// @brief Checks whether a histogram of a plan on a level would be the one already counted.
    /// @param level Buffer the histogram would be sampled from.
    /// @param plan Plan it would be sampled with, for the histogram's request.
    [[nodiscard]] bool isCurrent(const std::shared_ptr<const ImageBuffer>& level,
                                 const ProcessingPlan& plan) const;

    /// @brief Records the level and plan of a histogram just counted.
    /// @param level Buffer it was sampled from; kept alive, so that its address identifies it.
    /// @param plan Plan it was sampled with.
    void record(std::shared_ptr<const ImageBuffer> level, ProcessingPlan plan);

    /// @brief Forgets the last histogram, so that the next one is counted whatever it is.
    void clear() noexcept;

private:
    std::shared_ptr<const ImageBuffer> level_;
    std::optional<ProcessingPlan> plan_;
};

} // namespace arraw::app
