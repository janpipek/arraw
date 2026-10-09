#pragma once

#include "SettingPresentation.h"

#include <DevelopState.h>
#include <LocalAdjustments.h>
#include <SettingDescriptors.h>

#include <QString>

#include <string_view>

namespace arraw::app {

/// @brief Range of the Opacity row, in percent; the state stores 0 to 1.
inline constexpr SettingRange maskOpacityRange{0.0, 100.0};

/// @brief Finds the presentation of one local control (ADR 044).
///
/// A control with a global counterpart takes that one's label, unit, decimals and step, so the
/// list, the sliders and the history word a delta alike; its tool tip says the mask's own.
/// Relative temperature and tint, which have no counterpart, are "Temp" and "Tint". The range of
/// a row is never in the presentation: it is the local descriptor's
/// (::arraw::localAdjustmentDescriptors), narrower than the global one for Exposure.
/// @param localKey camelCase key of a row of ::arraw::localAdjustmentDescriptors.
/// @return The presentation, valid for the life of the program.
/// @throws std::out_of_range if @p localKey names no local control.
[[nodiscard]] const SettingPresentation& localPresentationOf(std::string_view localKey);

/// @brief Gives the presentation of the Opacity row, which shows the stored 0 to 1 as a percent.
[[nodiscard]] const SettingPresentation& maskOpacityPresentation();

/// @brief Gives the name a mask is shown under: its own, or its kind and place among that kind.
///
/// Translated, such as "Linear 2".
/// @param state State holding the mask.
/// @param id Mask to name.
/// @throws std::invalid_argument if @p id is not in the list.
[[nodiscard]] QString maskDisplayName(const DevelopState& state, LocalAdjustmentId id);

} // namespace arraw::app
