#pragma once

#include <GeometrySettings.h>
#include <ImageBuffer.h>

#include <array>

namespace arraw {

/// @brief Position in decoded image edge units, before camera orientation.
struct SourcePoint {
    double x = 0.0;
    double y = 0.0;
};

/// @brief Position in uncropped upright image edge units.
struct UprightPoint {
    double x = 0.0;
    double y = 0.0;
};

/// @brief Resolved orthogonal geometry and continuous framing for one render.
struct GeometryPlan {
    /// @brief Decoded raster dimensions.
    ImageSize sourceSize;
    /// @brief Row-major source-to-upright linear transform, about image centres.
    std::array<double, 4> matrix{1.0, 0.0, 0.0, 1.0};
    /// @brief Continuous upright bounding-box dimensions.
    double uprightWidth = 0.0;
    double uprightHeight = 0.0;
    /// @brief Resolved crop in upright edge units.
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
    /// @brief Raster dimensions after flooring continuous crop dimensions.
    ImageSize outputSize;

    /// @brief Maps a source edge position into the uncropped upright frame.
    [[nodiscard]] UprightPoint toUpright(SourcePoint point) const;
    /// @brief Maps an upright edge position back to decoded coordinates.
    [[nodiscard]] SourcePoint toSource(UprightPoint point) const;

    friend bool operator==(const GeometryPlan&, const GeometryPlan&) = default;
};

/// @brief Resolves orientation, straighten, flips and valid crop framing.
/// @throws std::invalid_argument if geometry values or constraints are invalid.
[[nodiscard]] GeometryPlan geometryPlanFor(ImageSize sourceSize, ImageOrientation orientation,
                                           const GeometrySettings& settings);

/// @brief Resamples developed float pixels through a resolved geometry plan.
/// @return Upright working pixels with no pending source orientation.
[[nodiscard]] ImageBuffer applyGeometry(ImageBuffer source, const GeometryPlan& plan);

} // namespace arraw
