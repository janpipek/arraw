#pragma once

#include <optional>

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

    friend bool operator==(const ColorSettings&, const ColorSettings&) = default;
};

} // namespace arraw
