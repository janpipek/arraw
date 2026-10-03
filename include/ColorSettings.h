#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace arraw {

/// @brief Where a photograph's white balance comes from.
enum class WhiteBalanceMode {
    /// @brief The light the camera recorded, left alone.
    ///
    /// The default, and the reason developing a photograph with no settings
    /// changes nothing about its colour.
    AsShot,

    /// @brief A light the photographer named instead.
    Custom,
};

/// @brief Stable names of the white balance modes, as documents and the command line spell them.
inline constexpr std::array<std::pair<WhiteBalanceMode, std::string_view>, 2> whiteBalanceModeNames{
    {
        {WhiteBalanceMode::AsShot, "asShot"},
        {WhiteBalanceMode::Custom, "custom"},
    }};

/// @brief Weakest and strongest Saturation and Vibrance arraw models.
///
/// Both run from fully desaturated through unchanged to double the colourfulness,
/// as Lightroom's sliders do (ADR 027).
inline constexpr float weakestSaturation = -100.0F;

/// @copydoc weakestSaturation
inline constexpr float strongestSaturation = 100.0F;

/// @brief Photographic colour adjustments in domain units.
struct ColorSettings {
    /// @brief Which light the photograph is balanced for.
    WhiteBalanceMode whiteBalance = WhiteBalanceMode::AsShot;

    /// @brief Temperature of that light in kelvin, when it is a custom one.
    ///
    /// Absent means the camera's own reading, which is also what a photograph
    /// falls back to when only the tint was moved. Meaningless without a
    /// sensor to measure against, so it applies to RAW files alone (ADR 008).
    std::optional<float> temperature = std::nullopt;

    /// @brief How far off the line of glowing-object colours that light sits.
    std::optional<float> tint = std::nullopt;

    /// @brief How much the colourfulness of every colour is raised or lowered.
    ///
    /// Scales Oklab chroma uniformly, holding lightness and hue: minus a
    /// hundred is grey, plus a hundred doubles the chroma (ADR 027). Zero
    /// leaves the photograph alone.
    float saturation = 0.0F;

    /// @brief How much the colourfulness of the muted colours is raised or lowered.
    ///
    /// Like Saturation, but weighted so that colours that are already vivid
    /// move less than muted ones.
    float vibrance = 0.0F;

    friend bool operator==(const ColorSettings&, const ColorSettings&) = default;
};

} // namespace arraw
