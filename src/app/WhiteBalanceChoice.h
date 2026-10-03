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

/// @brief Applies a combo entry to the settings.
/// @param settings Settings to start from.
/// @param choice Entry chosen.
/// @return As Shot with no values; a named light as Custom with both its values;
/// for the Custom entry, @p settings unchanged.
[[nodiscard]] ColorSettings withChoice(ColorSettings settings, WhiteBalanceChoice choice);

/// @brief Moves the temperature, leaving the tint where it is.
///
/// A value makes the mode Custom. Clearing it returns to As Shot when the tint
/// is not set either. In As Shot mode any stale values are ignored: the rows
/// showed the camera's reading, so that is what the other value stays at.
/// @param settings Settings to start from.
/// @param kelvin New temperature, or empty to follow the camera again.
[[nodiscard]] ColorSettings withTemperature(ColorSettings settings, std::optional<float> kelvin);

/// @brief Moves the tint, leaving the temperature where it is.
/// @copydetails withTemperature
[[nodiscard]] ColorSettings withTint(ColorSettings settings, std::optional<float> tint);

/// @brief Gives the light the Temp and Tint rows show.
/// @param settings Settings being shown.
/// @param asShot The light the decode balanced for, shown for whichever value is absent.
[[nodiscard]] ColourTemperature shownLight(const ColorSettings& settings, ColourTemperature asShot);

} // namespace arraw::app
