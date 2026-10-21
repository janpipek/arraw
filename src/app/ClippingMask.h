#pragma once

#include <QImage>

namespace arraw::app {

/// @brief Paints the clipped pixels of a picture, for the overlay over it (ADR 055, section 5).
///
/// A pixel is clipped if any of R, G, B is 255 (highlights) or 0 (shadows). Highlights are red
/// and shadows blue, as the theme sets them; a pixel that is both is painted as highlights when
/// those are on. Fully transparent pixels are left alone, as the histogram does not count them.
/// @param picture Picture in its output encoding, in any format.
/// @param shadows Whether to paint the shadow-clipped pixels.
/// @param highlights Whether to paint the highlight-clipped pixels.
/// @return An image of the picture's size in ::QImage::Format_ARGB32_Premultiplied, transparent
/// where nothing is painted; null for a null picture.
[[nodiscard]] QImage clippingMask(const QImage& picture, bool shadows, bool highlights);

} // namespace arraw::app
