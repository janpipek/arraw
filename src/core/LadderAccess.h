#pragma once

#include "CheckpointState.h"
#include "ProcessingPlan.h"

#include <CheckpointLadder.h>
#include <ImageBuffer.h>
#include <RenderCheckpoint.h>

#include <cstddef>
#include <memory>
#include <optional>

namespace arraw {

/// @brief Engine-side access to a ladder's rungs, for the render entries and the driver.
struct LadderAccess {
    /// @brief Binds a ladder to a source, dropping the rungs of any other.
    /// @param ladder Ladder to bind.
    /// @param source Buffer the next renders develop from; kept alive, so that its address
    /// identifies it.
    static void bind(CheckpointLadder& ladder, const std::shared_ptr<const ImageBuffer>& source);

    /// @brief Finds the deepest rung a render can resume from, dropping those it cannot.
    ///
    /// From the Resize rung down to the Denoise one: a rung the backend cannot
    /// resume from, or that ::arraw::staleReason refuses, is dropped; the first
    /// that passes is the answer. Shallower rungs than the answer stay: the
    /// answer's prefix matching means theirs do too.
    /// @param ladder Ladder to search.
    /// @param plan The render's whole plan.
    /// @param sourceSize Size of the source the render develops.
    /// @param holdsHere Callable `(const CheckpointState&) -> bool`: whether a rung's pixels live
    /// where this backend resumes from.
    /// @return The boundary of the rung to resume from, or empty for the source.
    template <typename HoldsHere>
    [[nodiscard]] static std::optional<Stage>
    deepestUsable(CheckpointLadder& ladder, const ProcessingPlan& plan, ImageSize sourceSize,
                  HoldsHere&& holdsHere) {
        for (std::size_t index = stageCount; index-- > 0;) {
            auto& slot = ladder.rungs_[index];
            if (!slot) {
                continue;
            }
            const CheckpointState& held = stateOf(*slot);
            if (holdsHere(held) && !staleReason(held, plan, sourceSize)) {
                return held.boundary;
            }
            slot.reset();
        }
        return std::nullopt;
    }

    /// @brief Gives the rung at a boundary.
    /// @pre The ladder holds one there.
    [[nodiscard]] static const RenderCheckpoint& rung(const CheckpointLadder& ladder,
                                                      Stage boundary);

    /// @brief Puts a rung at its boundary, replacing any.
    static void store(CheckpointLadder& ladder, const RenderCheckpoint& rung);
};

} // namespace arraw
