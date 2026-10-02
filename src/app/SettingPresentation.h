#pragma once

#include <SettingDescriptors.h>

#include <QString>

#include <span>
#include <string_view>

namespace arraw::app {

/// @brief Presentation of one setting, derived from the model (ADR 008).
struct SettingPresentation {
    /// @brief Localised name shown beside the control.
    QString label;

    /// @brief Suffix shown after the number, such as " EV"; may be empty.
    QString unit;

    /// @brief Digits shown after the decimal point.
    int decimals;

    /// @brief Distance between two slider positions, in the setting's own units.
    double step;

    /// @brief Localised one-sentence explanation for a photographer.
    QString toolTip;
};

/// @brief Gives the keys of the Tone group in the order the panel shows them.
[[nodiscard]] std::span<const std::string_view> toneKeys() noexcept;

/// @brief Finds the presentation of a setting.
/// @param key camelCase key of a setting the panel shows, such as one of ::arraw::app::toneKeys.
/// @return The presentation, valid for the life of the program.
/// @throws std::out_of_range if no presentation exists for @p key.
[[nodiscard]] const SettingPresentation& presentationOf(std::string_view key);

/// @brief Counts the slider steps across a range.
/// @param range Range of the setting.
/// @param step Distance between two steps, in the setting's units; positive.
/// @return The number of steps from minimum to maximum.
[[nodiscard]] int tickCount(const SettingRange& range, double step);

/// @brief Maps a value to the nearest slider step.
/// @param value Value in the setting's units.
/// @param range Range of the setting.
/// @param step Distance between two steps, in the setting's units; positive.
/// @return The step, clamped to 0 through tickCount().
[[nodiscard]] int tickOf(double value, const SettingRange& range, double step);

/// @brief Maps a slider step to its value.
/// @param tick Slider step; need not lie within the range.
/// @param range Range of the setting.
/// @param step Distance between two steps, in the setting's units; positive.
/// @return The value, clamped to the range.
[[nodiscard]] double valueOfTick(int tick, const SettingRange& range, double step);

/// @brief Reads the value a setting has in a default-constructed ::arraw::DevelopSettings.
/// @param descriptor Row of a float or double setting.
/// @return The default value.
/// @throws std::invalid_argument if the setting is neither float nor double.
[[nodiscard]] double defaultValueOf(const FieldDescriptor& descriptor);

} // namespace arraw::app
