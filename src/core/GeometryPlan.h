#pragma once

#include <GeometrySettings.h>
#include <ImageBuffer.h>

#include <array>
#include <optional>

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

/// @brief Axis-aligned rectangle in uncropped upright image edge units.
struct UprightBox {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;

    friend bool operator==(const UprightBox&, const UprightBox&) = default;
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

    /// @brief Gives the resolved crop as a box.
    [[nodiscard]] UprightBox crop() const noexcept {
        return {left, top, width, height};
    }

    /// @brief Whether this geometry leaves pixels exactly where they are.
    ///
    /// No rotation, flip or crop: the condition under which a backend skips
    /// its resample, shared so that the CPU and the GPU cannot disagree about
    /// when one happens.
    [[nodiscard]] bool isIdentity() const noexcept;

    friend bool operator==(const GeometryPlan&, const GeometryPlan&) = default;
};

/// @brief Resolves orientation, straighten, flips and valid crop framing.
/// @throws std::invalid_argument if geometry values or constraints are invalid.
[[nodiscard]] GeometryPlan geometryPlanFor(ImageSize sourceSize, ImageOrientation orientation,
                                           const GeometrySettings& settings);

/// @brief Gives the corners of the valid image content in the uncropped upright frame.
///
/// The decoded raster's corners, mapped: a rectangle, rotated by any straighten.
/// In the order of the source's top-left, top-right, bottom-right and
/// bottom-left corners, so consecutive corners share an edge.
[[nodiscard]] std::array<UprightPoint, 4> contentCorners(const GeometryPlan& plan);

/// @brief Tells whether a box lies inside valid image content (ADR 014).
///
/// Its upright frame is the plan's; the plan's own crop plays no part.
/// @param slack Distance a corner may lie outside, in edge units, for rounding.
[[nodiscard]] bool isInsideContent(const GeometryPlan& plan, const UprightBox& box,
                                   double slack = 1e-9);

/// @brief Fits a box into valid content as an explicit crop is fitted (ADR 014).
///
/// A box already inside is returned as it is. Otherwise it shrinks about its
/// centre, keeping its aspect; only when no positive box fits at that centre
/// is the largest retained size taken and its centre moved to the nearest
/// feasible point.
[[nodiscard]] UprightBox fittedToContent(const GeometryPlan& plan, UprightBox box);

/// @brief Moves a box the shortest distance that puts it inside valid content.
///
/// Its size is kept when it fits anywhere; otherwise it shrinks to the largest
/// that does, keeping its aspect, and that is moved instead. Distance is
/// measured in the source's axes, which an orthogonal map preserves, so the
/// box slides along an edge of the content rather than stopping at it.
[[nodiscard]] UprightBox shiftedIntoContent(const GeometryPlan& plan, UprightBox box);

/// @brief Gives the automatic framing of ADR 014, centred in the upright frame.
///
/// The largest box inside valid content: of any shape when @p ratio is empty,
/// else at that physical width-over-height ratio.
[[nodiscard]] UprightBox automaticCrop(const GeometryPlan& plan, std::optional<double> ratio);

/// @brief Resamples developed float pixels through a resolved geometry plan.
/// @return Upright working pixels with no pending source orientation.
[[nodiscard]] ImageBuffer applyGeometry(ImageBuffer source, const GeometryPlan& plan);

} // namespace arraw
