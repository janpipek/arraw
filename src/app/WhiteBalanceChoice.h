#pragma once

#include <ColorSettings.h>
#include <WhiteBalance.h>

#include <QString>

#include <array>
#include <optional>
#include <span>

namespace arraw::app {

/// @brief Entry of the white balance combo box.
///
/// As Shot is a mode of the settings; the named lights are values of Custom
/// (ADR 008); Custom itself is what the combo shows when the values match
/// none of them, and choosing it changes nothing.
enum class WhiteBalanceChoice {
    AsShot,
    Daylight,
    Cloudy,
    Shade,
    Tungsten,
    Fluorescent,
    Flash,
    Custom
};

/// @brief Named light a photographer can pick by name.
struct WhiteBalancePreset {
    /// @brief Entry the preset is shown as.
    WhiteBalanceChoice choice;

    /// @brief Untranslated name, for QCoreApplication::translate.
    const char* name;

    /// @brief Light the preset stands for.
    ColourTemperature light;
};

/// @brief Gives the named lights, in the order the combo box lists them.
///
/// Lightroom's values, because arraw's tint is on Lightroom's scale.
[[nodiscard]] std::span<const WhiteBalancePreset> whiteBalancePresets() noexcept;

/// @brief Gives every combo entry in the order the combo box lists them.
[[nodiscard]] std::span<const WhiteBalanceChoice> whiteBalanceChoices() noexcept;

/// @brief Gives the localised name of a combo entry.
[[nodiscard]] QString nameOf(WhiteBalanceChoice choice);

/// @brief Finds which entry the settings correspond to.
/// @param settings Colour settings to describe.
/// @return As Shot in that mode; else the first named light with exactly the
/// settings' temperature and tint (Daylight before Flash); else Custom.
[[nodiscard]] WhiteBalanceChoice choiceOf(const ColorSettings& settings);

/// @brief Gives the light a combo entry names.
/// @param choice Entry chosen.
/// @return The light of a named entry; nothing for As Shot and Custom.
[[nodiscard]] std::optional<ColourTemperature> lightOf(WhiteBalanceChoice choice);

/// @brief Gives the light the Temp and Tint rows show.
/// @param settings Settings being shown.
/// @param asShot The light the decode balanced for, shown for whichever value is absent.
[[nodiscard]] ColourTemperature shownLight(const ColorSettings& settings, ColourTemperature asShot);

} // namespace arraw::app
