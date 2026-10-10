#pragma once

#include <BrushStrokes.h>
#include <DevelopState.h>
#include <LocalAdjustmentEdits.h>
#include <SettingDescriptors.h>

#include <algorithm>
#include <memory>
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

/// @brief A stroke along a segment, with evenly spaced points.
inline Stroke paintedStroke(SensorPoint from, SensorPoint to, int points, float radius,
                            float hardness, float flow, bool erase = false) {
    Stroke stroke{radius, hardness, flow, erase, {}};
    for (int i = 0; i < points; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(points - 1);
        stroke.points.push_back({from.u + (to.u - from.u) * t, from.v + (to.v - from.v) * t});
    }
    return stroke;
}

/// @brief A brush mask of the given strokes.
inline BrushMask brushMaskWith(std::vector<Stroke> strokes) {
    return BrushMask{std::make_shared<const StrokeList>(std::move(strokes))};
}

/// @brief A state with a painted brush, a brush with an erase stroke, an inverted named brush
/// with no deltas, a disabled empty one, and a radial mask.
inline DevelopState stateWithBrushes() {
    DevelopState state;
    state = withLocalAdjustmentAdded(
        state, brushMaskWith({paintedStroke({0.1F, 0.2F}, {0.9F, 0.3F}, 12, 0.08F, 0.3F, 1.0F),
                              paintedStroke({0.15F, 0.8F}, {0.85F, 0.2F}, 12, 0.06F, 0.8F, 0.9F)}));
    state = withLocalAdjustmentAdded(
        state,
        brushMaskWith({paintedStroke({0.1F, 0.5F}, {0.9F, 0.5F}, 9, 0.1F, 0.5F, 1.0F),
                       paintedStroke({0.5F, 0.2F}, {0.5F, 0.9F}, 8, 0.05F, 0.5F, 1.0F, true)}));
    state = withLocalAdjustmentAdded(
        state, brushMaskWith({paintedStroke({0.3F, 0.5F}, {0.6F, 0.7F}, 6, 0.1F, 1.0F, 1.0F)}));
    state = withLocalAdjustmentAdded(state, BrushMask{});
    state = withLocalAdjustmentAdded(state, someRadial());
    const std::vector<LocalAdjustment> list = state.localAdjustments;
    state = withLocalDelta(state, list[0].id, "exposure", 0.6);
    state = withLocalDelta(state, list[0].id, "relativeTemperature", 40.0);
    state = withLocalDelta(state, list[1].id, "contrast", -30.0);
    state = withLocalAdjustmentInverted(state, list[2].id, true);
    state = withLocalAdjustmentRenamed(state, list[2].id, "Everything but the tree");
    state = withLocalDelta(state, list[2].id, "saturation", 30.0);
    state = withLocalAdjustmentEnabled(state, list[3].id, false);
    state = withLocalDelta(state, list[4].id, "dehaze", -30.0);
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
