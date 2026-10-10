#include "CheckpointLadder.h"

#include "BrushCoverage.h"
#include "LadderAccess.h"

#include <cstddef>

namespace arraw {

CheckpointLadder::CheckpointLadder(const CheckpointLadder& other)
    : source_(other.source_), rungs_(other.rungs_) {}

CheckpointLadder& CheckpointLadder::operator=(const CheckpointLadder& other) {
    if (this != &other) {
        source_ = other.source_;
        rungs_ = other.rungs_;
        coverage_.reset();
    }
    return *this;
}

void CheckpointLadder::clear() noexcept {
    coverage_.reset();
    for (auto& rung : rungs_) {
        rung.reset();
    }
    source_.reset();
}

bool CheckpointLadder::empty() const noexcept {
    for (const auto& rung : rungs_) {
        if (rung) {
            return false;
        }
    }
    return true;
}

bool CheckpointLadder::holds(Stage boundary) const noexcept {
    const auto index = static_cast<std::size_t>(boundary);
    return index < stageCount && rungs_[index].has_value();
}

void LadderAccess::bind(CheckpointLadder& ladder,
                        const std::shared_ptr<const ImageBuffer>& source) {
    if (source != ladder.source_) {
        ladder.clear();
        ladder.source_ = source;
    }
}

detail::CoverageResidency& LadderAccess::coverage(CheckpointLadder& ladder) {
    if (!ladder.coverage_) {
        ladder.coverage_ = std::make_shared<detail::CoverageResidency>();
    }
    return *ladder.coverage_;
}

void LadderAccess::dropCoverage(CheckpointLadder& ladder) noexcept {
    ladder.coverage_.reset();
}

const detail::CoverageResidency* LadderAccess::coverage(const CheckpointLadder& ladder) noexcept {
    return ladder.coverage_.get();
}

const RenderCheckpoint& LadderAccess::rung(const CheckpointLadder& ladder, Stage boundary) {
    return *ladder.rungs_[static_cast<std::size_t>(boundary)];
}

void LadderAccess::store(CheckpointLadder& ladder, const RenderCheckpoint& rung) {
    ladder.rungs_[static_cast<std::size_t>(rung.boundary())] = rung;
}

} // namespace arraw
