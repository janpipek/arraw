#include "DevelopState.h"

#include <SettingDescriptors.h>

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
}
