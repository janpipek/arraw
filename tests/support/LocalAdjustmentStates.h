#pragma once

#include <DevelopState.h>
#include <LocalAdjustmentEdits.h>
#include <SettingDescriptors.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace arraw::test {

/// @brief A linear mask that is neither at the defaults nor at any limit.
inline LinearMask someLinear() {
    return {.from = {0.2F, 0.1F}, .to = {0.35F, 0.8F}};
}

/// @brief A radial mask that is neither at the defaults nor at any limit.
inline RadialMask someRadial() {
    return {.centre = {0.4F, 0.6F},
            .radiusX = 0.3F,
            .radiusY = 0.15F,
            .angle = 30.0F,
            .feather = 0.25F};
}

/// @brief Gives every delta of an adjustment a distinct non-zero value inside its range.
inline void giveEveryDelta(LocalDeltas& deltas) {
    float value = 0.1F;
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        deltas.*descriptor.member =
            static_cast<float>(std::min(value, static_cast<float>(descriptor.range.maximum)));
        value += 3.7F;
    }
}

/// @brief A state with a disabled linear mask, an inverted named radial one, and a plain radial
/// one, each carrying deltas, and a counter above their ids.
inline DevelopState stateWithMasks() {
    DevelopState state;
    state = withLocalAdjustmentAdded(state, someLinear());
    state = withLocalAdjustmentAdded(state, someRadial());
    state = withLocalAdjustmentAdded(state, RadialMask{});
    const std::vector<LocalAdjustment> list = state.localAdjustments;
    state = withLocalAdjustmentEnabled(state, list[0].id, false);
    state = withLocalDelta(state, list[0].id, "exposure", 0.5);
    state = withLocalDelta(state, list[0].id, "dehaze", 20.0);
    state = withLocalAdjustmentInverted(state, list[1].id, true);
    state = withLocalAdjustmentRenamed(state, list[1].id, "Sky \"left\" & <more>");
    state = withLocalOpacity(state, list[1].id, 0.35F);
    state = withLocalDelta(state, list[1].id, "relativeTemperature", -42.5);
    state = withLocalDelta(state, list[1].id, "vibrance", 7.0);
    return state;
}

/// @brief A state whose one mask carries a non-zero value in every delta.
inline DevelopState stateWithEveryDelta() {
    LocalAdjustment adjustment;
    adjustment.shape = someRadial();
    giveEveryDelta(adjustment.deltas);
    return withLocalAdjustmentAdded(DevelopState{}, std::move(adjustment));
}

} // namespace arraw::test
