#pragma once

#include <cstdint>

namespace arraw {

/// @brief A rectangle of pixels in a raster.
struct PixelRect {
    std::uint32_t x;      ///< First column.
    std::uint32_t y;      ///< First row.
    std::uint32_t width;  ///< Columns.
    std::uint32_t height; ///< Rows.
};

} // namespace arraw
