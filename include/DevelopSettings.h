#pragma once

#include <optional>

#include <GeometrySettings.h>
#include <ToneSettings.h>

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

/// @brief Photographic settings applied to one photograph, in domain units.
///
/// Plain values: presentation decides how to show them, and a descriptor table
/// beside them carries ranges, defaults and applicability (ADR 008).
struct DevelopSettings {
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

    /// @brief Orientation, straightening and crop; not yet consumed by rendering.
    GeometrySettings geometry{};

    /// @brief Exposure, tonal shaping and highlight roll-off.
    ToneSettings tone{};

    friend bool operator==(const DevelopSettings&, const DevelopSettings&) = default;
};

} // namespace arraw
