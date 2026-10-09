#include "LocalAdjustmentEdits.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace arraw;

namespace {

/// @brief Finds an adjustment's place in the list, or refuses an id that is not there.
std::size_t indexOf(const DevelopState& state, LocalAdjustmentId id) {
    const auto found = std::ranges::find(state.localAdjustments, id, &LocalAdjustment::id);
    if (found == state.localAdjustments.end()) {
        throw std::invalid_argument(std::format("no local adjustment has the id {}", id.value));
    }
    return static_cast<std::size_t>(found - state.localAdjustments.begin());
}

/// @brief Refuses an addition the list cannot take.
void requireRoom(const DevelopState& state) {
    if (state.localAdjustments.size() >= maximumLocalAdjustments) {
        throw std::invalid_argument(std::format("a photograph holds at most {} local adjustments",
                                                maximumLocalAdjustments));
    }
    if (!canAddLocalAdjustment(state)) {
        throw std::invalid_argument("the local adjustment ids have run out");
    }
}

/// @brief Hands out the counter's value and moves the counter on.
LocalAdjustmentId takeId(DevelopState& state) {
    const LocalAdjustmentId id = state.nextLocalAdjustmentId;
    ++state.nextLocalAdjustmentId.value;
    return id;
}

} // namespace

bool arraw::canAddLocalAdjustment(const DevelopState& state) noexcept {
    return state.localAdjustments.size() < maximumLocalAdjustments &&
           state.nextLocalAdjustmentId.value != 0 &&
           state.nextLocalAdjustmentId.value != std::numeric_limits<std::uint32_t>::max();
}

const LocalAdjustment* arraw::findLocalAdjustment(const DevelopState& state,
                                                  LocalAdjustmentId id) noexcept {
    const auto found = std::ranges::find(state.localAdjustments, id, &LocalAdjustment::id);
    return found == state.localAdjustments.end() ? nullptr : &*found;
}

DevelopState arraw::withLocalAdjustmentAdded(DevelopState state, LocalAdjustment adjustment) {
    requireRoom(state);
    adjustment = normalised(std::move(adjustment));
    adjustment.id = takeId(state);
    state.localAdjustments.push_back(std::move(adjustment));
    return state;
}

DevelopState arraw::withLocalAdjustmentAdded(DevelopState state, Mask shape) {
    LocalAdjustment adjustment;
    adjustment.shape = std::move(shape);
    return withLocalAdjustmentAdded(std::move(state), std::move(adjustment));
}

DevelopState arraw::withLocalAdjustmentDuplicated(DevelopState state, LocalAdjustmentId id) {
    const std::size_t index = indexOf(state, id);
    requireRoom(state);
    LocalAdjustment copy = state.localAdjustments[index];
    copy.id = takeId(state);
    state.localAdjustments.insert(
        state.localAdjustments.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(copy));
    return state;
}

DevelopState arraw::withLocalAdjustmentRemoved(DevelopState state, LocalAdjustmentId id) {
    const std::size_t index = indexOf(state, id);
    state.localAdjustments.erase(state.localAdjustments.begin() +
                                 static_cast<std::ptrdiff_t>(index));
    return state;
}

DevelopState arraw::withLocalAdjustmentReordered(DevelopState state, LocalAdjustmentId id,
                                                 std::size_t index) {
    const std::size_t from = indexOf(state, id);
    if (index >= state.localAdjustments.size()) {
        throw std::invalid_argument(std::format("there is no position {} among {} adjustments",
                                                index, state.localAdjustments.size()));
    }
    const auto first = state.localAdjustments.begin();
    if (from < index) {
        std::rotate(first + static_cast<std::ptrdiff_t>(from),
                    first + static_cast<std::ptrdiff_t>(from) + 1,
                    first + static_cast<std::ptrdiff_t>(index) + 1);
    } else {
        std::rotate(first + static_cast<std::ptrdiff_t>(index),
                    first + static_cast<std::ptrdiff_t>(from),
                    first + static_cast<std::ptrdiff_t>(from) + 1);
    }
    return state;
}

DevelopState arraw::withLocalAdjustmentRenamed(DevelopState state, LocalAdjustmentId id,
                                               std::string name) {
    LocalAdjustment& adjustment = state.localAdjustments[indexOf(state, id)];
    adjustment.name = std::move(name);
    adjustment = normalised(std::move(adjustment));
    return state;
}

DevelopState arraw::withLocalAdjustmentEnabled(DevelopState state, LocalAdjustmentId id,
                                               bool enabled) {
    state.localAdjustments[indexOf(state, id)].enabled = enabled;
    return state;
}

DevelopState arraw::withLocalAdjustmentInverted(DevelopState state, LocalAdjustmentId id,
                                                bool invert) {
    state.localAdjustments[indexOf(state, id)].invert = invert;
    return state;
}

DevelopState arraw::withLocalOpacity(DevelopState state, LocalAdjustmentId id, float opacity) {
    LocalAdjustment& adjustment = state.localAdjustments[indexOf(state, id)];
    adjustment.opacity = opacity;
    adjustment = normalised(std::move(adjustment));
    return state;
}

DevelopState arraw::withLocalDelta(DevelopState state, LocalAdjustmentId id, std::string_view key,
                                   double value) {
    const LocalDescriptor* descriptor = findLocalDescriptor(key);
    if (descriptor == nullptr) {
        throw std::invalid_argument(std::format("no local control is named \"{}\"", key));
    }
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::format("{} is not a finite number", key));
    }
    LocalAdjustment& adjustment = state.localAdjustments[indexOf(state, id)];
    adjustment.deltas.*descriptor->member =
        static_cast<float>(std::clamp(value, descriptor->range.minimum, descriptor->range.maximum));
    return state;
}

DevelopState arraw::withLocalShape(DevelopState state, LocalAdjustmentId id, Mask shape) {
    LocalAdjustment& adjustment = state.localAdjustments[indexOf(state, id)];
    if (shape.index() != adjustment.shape.index()) {
        throw std::invalid_argument(std::format("a {} mask cannot take the geometry of a {} mask",
                                                maskTypeName(adjustment.shape),
                                                maskTypeName(shape)));
    }
    adjustment.shape = normalised(shape);
    return state;
}

std::size_t arraw::maskOrdinal(const DevelopState& state, LocalAdjustmentId id) {
    const std::size_t index = indexOf(state, id);
    const std::size_t kind = state.localAdjustments[index].shape.index();
    return static_cast<std::size_t>(
        std::count_if(state.localAdjustments.begin(),
                      state.localAdjustments.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                      [&](const LocalAdjustment& other) { return other.shape.index() == kind; }));
}

std::string arraw::defaultMaskName(const DevelopState& state, LocalAdjustmentId id) {
    const LocalAdjustment& adjustment = state.localAdjustments[indexOf(state, id)];
    return std::format("{} {}", adjustment.shape.index() == 0 ? "Linear" : "Radial",
                       maskOrdinal(state, id));
}

std::string arraw::displayedMaskName(const DevelopState& state, LocalAdjustmentId id) {
    const LocalAdjustment& adjustment = state.localAdjustments[indexOf(state, id)];
    return adjustment.name.empty() ? defaultMaskName(state, id) : adjustment.name;
}
