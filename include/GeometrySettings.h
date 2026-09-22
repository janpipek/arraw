#pragma once

#include <optional>
#include <variant>

namespace arraw {

/// @brief User rotation clockwise from the camera's declared orientation.
enum class QuarterTurn { None, Clockwise90, Clockwise180, Clockwise270 };

/// @brief Counterclockwise and clockwise limits of straightening, in degrees.
inline constexpr double minimumStraighten = -45.0;

/// @copydoc minimumStraighten
inline constexpr double maximumStraighten = 45.0;

/// @brief Unconstrained crop aspect when resizing.
struct FreeCropAspect {
    friend bool operator==(const FreeCropAspect&, const FreeCropAspect&) = default;
};

/// @brief Original image aspect after camera orientation and user quarter-turns.
struct OriginalCropAspect {
    friend bool operator==(const OriginalCropAspect&, const OriginalCropAspect&) = default;
};

/// @brief Fixed physical width-to-height ratio, including portrait or landscape.
struct CropRatio {
    /// @brief Positive finite width divided by height; 1 is square, 1.5 is 3:2.
    double widthOverHeight = 1.0;

    friend bool operator==(const CropRatio&, const CropRatio&) = default;
};

/// @brief Per-photograph aspect constraint, independent of the selected rectangle.
using CropAspect = std::variant<FreeCropAspect, OriginalCropAspect, CropRatio>;

/// @brief Crop edges normalised to the uncropped upright bounding rectangle.
///
/// The origin is the top left; x increases right and y increases down. These
/// are continuous image edges, not pixel centres. Finite edges satisfy
/// 0 <= left < right <= 1 and 0 <= top < bottom <= 1. The rectangle must also
/// lie wholly inside valid image content, not merely its bounding rectangle.
/// See ADR 014 for the frame, aspect and fitting contract.
struct UprightCropRect {
    /// @brief Left edge as a fraction of upright width.
    double left = 0.0;
    /// @brief Top edge as a fraction of upright height.
    double top = 0.0;
    /// @brief Right edge as a fraction of upright width.
    double right = 1.0;
    /// @brief Bottom edge as a fraction of upright height.
    double bottom = 1.0;

    friend bool operator==(const UprightCropRect&, const UprightCropRect&) = default;
};

/// @brief Automatic or explicit framing and its remembered aspect constraint.
struct CropSettings {
    /// @brief Explicit selection, or automatic largest valid framing when absent.
    std::optional<UprightCropRect> rectangle = std::nullopt;

    /// @brief Aspect constraint retained across edits and reopening.
    CropAspect aspect = FreeCropAspect{};

    friend bool operator==(const CropSettings&, const CropSettings&) = default;
};

/// @brief User geometry relative to the camera orientation, followed by cropping.
///
/// Settings only: the current renderer does not consume these values yet.
/// Camera orientation is always honoured and is metadata, not an override
/// here. The order is camera orientation, straighten, user quarter-turn,
/// horizontal flip, vertical flip, then crop (ADR 014).
struct GeometrySettings {
    /// @brief Clockwise rotation in exact quarter-turns.
    QuarterTurn rotation = QuarterTurn::None;

    /// @brief Horizontal reflection in the final upright axes.
    bool flipHorizontal = false;

    /// @brief Vertical reflection in the final upright axes.
    bool flipVertical = false;

    /// @brief Fine clockwise rotation in degrees, before user flips, from -45 to 45.
    double straighten = 0.0;

    /// @brief Final framing, always constrained to valid image content.
    CropSettings crop{};

    friend bool operator==(const GeometrySettings&, const GeometrySettings&) = default;
};

} // namespace arraw
