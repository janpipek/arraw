#include "ClippingMask.h"

#include "ui/ThemeColors.h"

#include <bit>
#include <cstdint>

namespace arraw::app {

QImage clippingMask(const QImage& picture, bool shadows, bool highlights) {
    if (picture.isNull()) {
        return {};
    }
    // Shares the pixels when the picture already is RGBA8888, as the preview's is.
    const QImage source = picture.convertToFormat(QImage::Format_RGBA8888);
    QImage mask(source.size(), QImage::Format_ARGB32_Premultiplied);
    const QRgb highlightPaint = theme::clippingHighlights.rgba();
    const QRgb shadowPaint = theme::clippingShadows.rgba();
    // Held in locals: the compiler cannot tell the stores below from changes of the image's own
    // size, and would reread it for every pixel.
    const int width = source.width();
    const int height = source.height();
    // RGBA8888 is the bytes r, g, b, a in memory: as a word on a little-endian machine, red is
    // the lowest byte. One word a pixel and no branches is 2 to 3 times faster than bytes.
    static_assert(std::endian::native == std::endian::little);
    const bool paintHighlights = highlights;
    const bool paintShadows = shadows;
    for (int y = 0; y < height; ++y) {
        const auto* in = reinterpret_cast<const std::uint32_t*>(source.constScanLine(y));
        auto* out = reinterpret_cast<std::uint32_t*>(mask.scanLine(y));
        // Every pixel is written once, so the rows need no clearing first.
        for (int x = 0; x < width; ++x) {
            const std::uint32_t pixel = in[x];
            const std::uint32_t r = pixel & 0xFFU;
            const std::uint32_t g = (pixel >> 8) & 0xFFU;
            const std::uint32_t b = (pixel >> 16) & 0xFFU;
            const bool opaque = (pixel >> 24) != 0U;
            const bool high =
                paintHighlights && opaque && ((r == 255U) | (g == 255U) | (b == 255U));
            const bool low = paintShadows && opaque && ((r == 0U) | (g == 0U) | (b == 0U));
            out[x] = high ? highlightPaint : (low ? shadowPaint : 0U);
        }
    }
    return mask;
}

} // namespace arraw::app
