#include "DevelopState.h"

#include <SettingDescriptors.h>

void arraw::validate(const DevelopState& state) {
    validate(state.settings);
}
