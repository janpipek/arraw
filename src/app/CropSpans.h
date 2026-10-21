#pragma once

#include <OutputHistogram.h>

#include <QPointF>
#include <QSize>

#include <array>
#include <vector>

namespace arraw::app {

/// @brief Finds the pixels of an image that lie in a convex quadrilateral, row by row.
///
/// A pixel is inside if its centre, `(x + 0.5, y + 0.5)`, is. The rule for the edges is the usual
/// half-open one of the even-odd crossing test: a centre exactly on a left or top edge is inside,
/// one on a right or bottom edge is outside. The corners may come in either winding.
/// @param image Pixel dimensions of the image.
/// @param quad Corners in the image's pixel coordinates, where pixel `(x, y)` covers `[x, x + 1)`
/// by `[y, y + 1)`; they may lie outside the image.
/// @return Exactly one span per row, in the form ::arraw::countOutputCodes takes; an empty span
/// for a row with no pixel inside.
[[nodiscard]] std::vector<RowSpan> cropSpans(QSize image, const std::array<QPointF, 4>& quad);

} // namespace arraw::app
