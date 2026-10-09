#include "CheckpointLadder.h"

#include "LadderAccess.h"

#include <cstddef>

namespace arraw {

void CheckpointLadder::clear() noexcept {
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

const RenderCheckpoint& LadderAccess::rung(const CheckpointLadder& ladder, Stage boundary) {
    return *ladder.rungs_[static_cast<std::size_t>(boundary)];
}

void LadderAccess::store(CheckpointLadder& ladder, const RenderCheckpoint& rung) {
    ladder.rungs_[static_cast<std::size_t>(rung.boundary())] = rung;
}

} // namespace arraw
