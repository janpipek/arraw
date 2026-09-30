#pragma once

#include <ColorSettings.h>
#include <GeometrySettings.h>
#include <ToneSettings.h>

namespace arraw {

/// @brief Photographic settings applied to one photograph, in domain units.
///
/// Plain values: presentation decides how to show them, and the descriptor table
/// beside them (SettingDescriptors.h) carries ranges, groups and applicability
/// (ADR 008). A new leaf field needs a row there.
struct DevelopSettings {
    /// @brief White-balance mode, temperature and tint.
    ColorSettings color{};

    /// @brief Orientation, straightening and crop.
    GeometrySettings geometry{};

    /// @brief Exposure, tonal shaping and highlight roll-off.
    ToneSettings tone{};

    friend bool operator==(const DevelopSettings&, const DevelopSettings&) = default;
};

} // namespace arraw
