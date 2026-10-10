#pragma once

#include "BrushStrokes.h"

#include <ImageBuffer.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace arraw {

/// Smallest dab radius the pixel loop draws, in raster pixels.
///
/// Coverage is sampled at pixel centres, so a dab thinner than a pixel or two would fall between
/// them. A smaller brush is drawn this wide with its flow scaled by the ratio of the areas.
/// Measured: from 1.5 the weight of a hardness 1 line varies by 14% or less with its sub-pixel
/// position, against 59% from 1.
inline constexpr double minimumDrawnRadius = 1.5;

/// Centre of one dab, in raster pixels (pixel i spans [i, i+1)).
struct DabCentre {
    float x; ///< Horizontal position.
    float y; ///< Vertical position.
};

/// A stroke resolved against a raster size: what the pixel loop reads.
struct PlacedStroke {
    float radius; ///< Dab radius drawn, in raster pixels: the long edge times radius, at least
                  ///< ::arraw::minimumDrawnRadius.
    float inner;  ///< Radius of the fully opaque core in raster pixels.
    float flow; ///< Opacity of one dab, scaled by the ratio of the areas when the radius was raised
                ///< to ::arraw::minimumDrawnRadius (1.5 px).
    bool erase; ///< Whether the stroke removes coverage.
    std::vector<DabCentre> dabs; ///< Dabs, in path order.
    std::int64_t left;           ///< First pixel column of the conservative box of all dabs.
    std::int64_t top;            ///< First pixel row of the box.
    std::int64_t right;          ///< Column past the box (half-open), unclipped.
    std::int64_t bottom;         ///< Row past the box (half-open), unclipped.
};

/// A rectangle of pixels in a raster.
struct PixelRect {
    std::uint32_t x;      ///< First column.
    std::uint32_t y;      ///< First row.
    std::uint32_t width;  ///< Columns.
    std::uint32_t height; ///< Rows.
};

/// Float coverage of a whole raster, row-major.
struct CoveragePlane {
    ImageSize size;            ///< Pixel dimensions.
    std::vector<float> values; ///< Coverage, 0 to 1, size.width times size.height values.

    /// Compares the bits of the values, so that equal planes are the same to the last bit.
    friend bool operator==(const CoveragePlane& a, const CoveragePlane& b) {
        return a.size == b.size && a.values.size() == b.values.size() &&
               (a.values.empty() || std::memcmp(a.values.data(), b.values.data(),
                                                a.values.size() * sizeof(float)) == 0);
    }
};

/// Places the dabs of a stroke along its path.
///
/// One dab every quarter of the radius along the path, measured in the long-edge metric
/// of @p raster, and a last one at the end of the path when the spacing did not land there.
/// @param raster Raster the dabs are for.
/// @param endDab Whether to add the last dab at the end of the path; without it the dabs are
/// those that stay the same when the path grows (a live stroke's settled part).
/// @return Dab centres in raster pixels, at least one.
[[nodiscard]] std::vector<DabCentre> dabCentres(const Stroke& stroke, ImageSize raster,
                                                bool endDab = true);

/// Resolves one stroke against a raster size.
[[nodiscard]] PlacedStroke placedStroke(const Stroke& stroke, ImageSize raster);

/// Resolves the strokes of a list from @p first on.
[[nodiscard]] std::vector<PlacedStroke> placedStrokes(const StrokeList& strokes, ImageSize raster,
                                                      std::size_t first = 0);

/// Half-open pixel range [first, last) along one axis that a dab at @p centre can touch.
struct PixelSpan {
    std::int64_t first; ///< First pixel.
    std::int64_t last;  ///< Pixel past the last.
};

/// Finds the pixels along one axis a dab can reach, a little conservatively.
[[nodiscard]] PixelSpan dabSpan(float centre, float radius) noexcept;

/// Applies placed strokes, in order, onto coverage already held for a region.
/// @param strokes Strokes in painting order.
/// @param region Pixels the values are for, in raster coordinates.
/// @param values region.width * region.height floats, row-major, region-relative.
void paintRegion(std::span<const PlacedStroke> strokes, PixelRect region, std::span<float> values);

/// Draws all strokes onto zero over the whole raster, banded by rows: the reference.
/// @throws std::invalid_argument for an empty size or a rasteriser other than 1.
[[nodiscard]] CoveragePlane rasteriseBrush(const StrokeList& strokes, ImageSize raster);

} // namespace arraw
