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

/// @brief Darkest and brightest Exposure arraw models, in EV.
///
/// Named here rather than left to whatever a caller happens to pass; they
/// become rows in the descriptor table of ADR 008 when it exists.
inline constexpr float darkestExposure = -5.0F;

/// @copydoc darkestExposure
inline constexpr float brightestExposure = 5.0F;

/// @brief Photographic settings applied to one photograph, in domain units.
///
/// Plain values: presentation decides how to show them, and a descriptor table
/// beside them carries ranges, defaults and applicability (ADR 008).
struct DevelopSettings {
    /// @brief Exposure adjustment, in EV.
    float exposure = 0.0F;

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

    friend bool operator==(const DevelopSettings&, const DevelopSettings&) = default;
};

} // namespace arraw
