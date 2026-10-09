#pragma once

#include <CropGeometry.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <LocalAdjustments.h>

#include <array>
#include <cstdint>
#include <vector>

/// @file
/// @brief Where the corrected frame lies in the developed picture (ADR 044, section 4).
///
/// A mask is stored in the corrected frame, the photograph as decoded; a render shows the
/// developed frame, turned, straightened and cropped. A front end that draws a mask over the
/// picture, or reads one off a click, needs the map between the two, and the coverage the engine
/// would give a mask. Both are here, over the same geometry the render resolves.

namespace arraw {

/// @brief A position in the corrected frame in double precision.
///
/// ::arraw::CorrectedPoint stores floats; a drag computes in doubles and narrows once.
struct CorrectedPosition {
    double u = 0.0; ///< Position across, 0 at the left edge and 1 at the right.
    double v = 0.0; ///< Position down, 0 at the top edge and 1 at the bottom.

    constexpr CorrectedPosition() noexcept = default;
    /// @brief Creates a position from its two numbers.
    constexpr CorrectedPosition(double across, double down) noexcept : u(across), v(down) {}
    /// @brief Widens a stored point.
    constexpr CorrectedPosition(CorrectedPoint point) noexcept : u(point.u), v(point.v) {}

    /// @brief Gives the position as a stored point, narrowed to float.
    [[nodiscard]] constexpr CorrectedPoint asPoint() const noexcept {
        return {static_cast<float>(u), static_cast<float>(v)};
    }

    friend bool operator==(const CorrectedPosition&, const CorrectedPosition&) = default;
};

/// @brief A position in the developed frame: the upright, straightened, cropped picture a render
/// shows, normalised per axis to the crop.
///
/// 0 to 1 is on the picture; outside is off it.
struct DevelopedPoint {
    double x = 0.0; ///< Position across, 0 at the left edge of the crop and 1 at the right.
    double y = 0.0; ///< Position down, 0 at the top edge of the crop and 1 at the bottom.

    friend bool operator==(const DevelopedPoint&, const DevelopedPoint&) = default;
};

/// @brief A rectangle of the developed frame, in the units of ::arraw::DevelopedPoint.
struct DevelopedRegion {
    double left = 0.0;   ///< Left edge.
    double top = 0.0;    ///< Top edge.
    double width = 1.0;  ///< Width.
    double height = 1.0; ///< Height.

    friend bool operator==(const DevelopedRegion&, const DevelopedRegion&) = default;
};

/// @brief A position in the long-edge metric of the corrected frame (ADR 044, section 4).
///
/// The corrected frame scaled by one factor on both axes, so that its longer side is 1: distances
/// and angles measured here are the photograph's own. Circles are circles in it.
struct LongEdgePoint {
    double x = 0.0; ///< Position across, `u * width / longEdge`.
    double y = 0.0; ///< Position down, `v * height / longEdge`.

    friend bool operator==(const LongEdgePoint&, const LongEdgePoint&) = default;
};

/// @brief Placement of a photograph's corrected frame in its developed frame under a geometry.
///
/// An affine map both ways: turns, flips, straighten and crop compose to a similarity in pixels
/// (ADR 009, ADR 044, section 4), so a circle of the long-edge metric is a circle in the
/// developed picture. Built from the full source's shape, never a reduced one; everything is in
/// fractions, so a preview at any level agrees.
class DevelopedFrameMap {
public:
    /// @brief Resolves a geometry on a photograph.
    /// @param source Shape of the photograph; not empty.
    /// @param geometry Geometry to place the frame under.
    /// @throws std::invalid_argument as ::arraw::cropFrameFor does.
    DevelopedFrameMap(SourceShape source, const GeometrySettings& geometry);

    /// @brief Gives the position in the developed frame of a corrected position.
    [[nodiscard]] DevelopedPoint developedFrom(CorrectedPosition position) const noexcept;

    /// @brief Gives the corrected position at a position in the developed frame.
    [[nodiscard]] CorrectedPosition correctedFrom(DevelopedPoint point) const noexcept;

    /// @brief Gives the long-edge position of a corrected position.
    [[nodiscard]] LongEdgePoint longEdgeFrom(CorrectedPosition position) const noexcept;

    /// @brief Gives the corrected position at a long-edge position.
    [[nodiscard]] CorrectedPosition correctedFromLongEdge(LongEdgePoint point) const noexcept;

    /// @brief Gives the developed frame's width in source pixels (the continuous crop).
    [[nodiscard]] double width() const noexcept {
        return width_;
    }

    /// @brief Gives the developed frame's height in source pixels (the continuous crop).
    [[nodiscard]] double height() const noexcept {
        return height_;
    }

    /// @brief Gives one long-edge unit in source pixels: the source's longer side.
    [[nodiscard]] double longEdge() const noexcept {
        return longEdge_;
    }

    /// @brief Gives the shape of the photograph this map was made for.
    [[nodiscard]] const SourceShape& source() const noexcept {
        return source_;
    }

private:
    SourceShape source_;
    double width_ = 0.0;
    double height_ = 0.0;
    double longEdge_ = 0.0;
    /// Row-major 2x2 matrix and offset of corrected to developed: `d = forward * (u, v) + shift`.
    std::array<double, 4> forward_{};
    std::array<double, 2> shift_{};
    /// Row-major inverse of `forward_`.
    std::array<double, 4> backward_{};
};

/// @brief Weights of one mask over a grid laid on a region of the developed frame.
struct MaskCoverage {
    ImageSize size;                    ///< Grid size.
    std::vector<std::uint8_t> weights; ///< Row-major, `round(255 * w)` at each cell's centre.
};

/// @brief Largest side of a coverage grid.
inline constexpr std::uint32_t maximumCoverageSide = 4096;

/// @brief Evaluates a mask's weight at the centres of a grid over a region of the developed
/// frame, as development does (ADR 044, section 4), with Invert.
///
/// Enabled, Opacity and the deltas play no part, so a mask that does nothing yet still shows
/// where it would act. The weight is the engine's own (::arraw::maskWeight) resolved against the
/// full source, so a hard edge's one-pixel antialiasing is the export's. Cells off the picture
/// are evaluated too: the mask extends past the frame.
/// @param adjustment Adjustment whose shape and Invert to evaluate.
/// @param frame Placement of the corrected frame in the developed one.
/// @param region Rectangle of the developed frame the grid covers; finite, not empty.
/// @param size Grid size; each side 1 to ::arraw::maximumCoverageSide.
/// @throws std::invalid_argument if @p size or @p region is empty or not valid, or the shape
/// cannot be normalised (::arraw::normalised(const Mask&)).
[[nodiscard]] MaskCoverage maskCoverage(const LocalAdjustment& adjustment,
                                        const DevelopedFrameMap& frame, DevelopedRegion region,
                                        ImageSize size);

} // namespace arraw
