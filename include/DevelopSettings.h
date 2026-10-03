#pragma once

#include <BlackAndWhiteSettings.h>
#include <ColorSettings.h>
#include <GeometrySettings.h>
#include <HslSettings.h>
#include <ToneCurveSettings.h>
#include <ToneSettings.h>

namespace arraw {

/// @brief Photographic settings applied to one photograph, in domain units.
///
/// Plain values: presentation decides how to show them, and the descriptor table
/// beside them (SettingDescriptors.h) carries ranges, groups and applicability
/// (ADR 008). A new leaf field needs a row there.
struct DevelopSettings {
    /// @brief White-balance mode, temperature, tint, saturation and vibrance.
    ColorSettings color{};

    /// @brief Orientation, straightening and crop.
    GeometrySettings geometry{};

    /// @brief Exposure, tonal shaping and highlight roll-off.
    ToneSettings tone{};

    /// @brief Luma, red, green and blue curves over the tone.
    ToneCurveSettings toneCurve{};

    /// @brief Hue, saturation and luminance of eight bands of hues.
    HslSettings hsl{};

    /// @brief Conversion to grey and the mix of hues it is made from.
    BlackAndWhiteSettings blackAndWhite{};

    friend bool operator==(const DevelopSettings&, const DevelopSettings&) = default;
};

} // namespace arraw
