#pragma once

namespace arraw {

/// @brief Photographic settings applied to one photograph, in domain units.
///
/// Plain values: presentation decides how to show them, and a descriptor table
/// beside them carries ranges, defaults and applicability (ADR 008).
struct DevelopSettings {
    /// @brief Exposure adjustment, in EV.
    float exposure = 0.0F;

    friend bool operator==(const DevelopSettings&, const DevelopSettings&) = default;
};

} // namespace arraw
