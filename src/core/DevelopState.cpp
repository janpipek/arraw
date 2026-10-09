#include "DevelopState.h"

#include <SettingDescriptors.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <variant>

arraw::DevelopState arraw::defaultStateFor(const ColorEncoding& encoding) {
    DevelopState state;
    if (std::holds_alternative<CameraNative>(encoding)) {
        state.settings.noiseReduction.color = rawDefaultColorNoiseReduction;
    }
    return state;
}

void arraw::validate(const DevelopState& state) {
    validate(state.settings);
    if (state.nextLocalAdjustmentId.value == 0) {
        throw std::invalid_argument("the local adjustment counter starts at 1, not 0");
    }
    if (state.localAdjustments.size() > maximumLocalAdjustments) {
        throw std::invalid_argument(std::format("a photograph holds at most {} local adjustments, "
                                                "not {}",
                                                maximumLocalAdjustments,
                                                state.localAdjustments.size()));
    }
    for (std::size_t i = 0; i < state.localAdjustments.size(); ++i) {
        const LocalAdjustment& adjustment = state.localAdjustments[i];
        if (adjustment.id.value == 0) {
            throw std::invalid_argument(
                std::format("local adjustment {} has the id 0, which names none", i + 1));
        }
        if (adjustment.id >= state.nextLocalAdjustmentId) {
            throw std::invalid_argument(
                std::format("local adjustment {} has the id {}, not below the counter {}", i + 1,
                            adjustment.id.value, state.nextLocalAdjustmentId.value));
        }
        const bool repeated = std::any_of(
            state.localAdjustments.begin(),
            state.localAdjustments.begin() + static_cast<std::ptrdiff_t>(i),
            [&](const LocalAdjustment& earlier) { return earlier.id == adjustment.id; });
        if (repeated) {
            throw std::invalid_argument(
                std::format("the local adjustment id {} is used twice", adjustment.id.value));
        }
        try {
            validate(adjustment);
        } catch (const std::invalid_argument& problem) {
            throw std::invalid_argument(std::format("local adjustment {} (id {}): {}", i + 1,
                                                    adjustment.id.value, problem.what()));
        }
    }
}
