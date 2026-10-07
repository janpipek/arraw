#include "CurveHistogramRefresh.h"

#include <utility>

namespace arraw::app {

bool CurveHistogramRefresh::isCurrent(const std::shared_ptr<const ImageBuffer>& level,
                                      const ProcessingPlan& plan) const {
    return plan_ && level_ && level == level_ && sameAtTap(*plan_, plan, Tap::CurveInput);
}

void CurveHistogramRefresh::record(std::shared_ptr<const ImageBuffer> level, ProcessingPlan plan) {
    level_ = std::move(level);
    plan_ = std::move(plan);
}

void CurveHistogramRefresh::clear() noexcept {
    level_.reset();
    plan_.reset();
}

} // namespace arraw::app
