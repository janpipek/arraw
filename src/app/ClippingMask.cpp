#include "ClippingMask.h"

#include "ui/ThemeColors.h"

namespace arraw::app {

QImage clippingMask(const QImage& picture, bool shadows, bool highlights) {
    if (picture.isNull()) {
        return {};
    }
    // Shares the pixels when the picture already is RGBA8888, as the preview's is.
    const QImage source = picture.convertToFormat(QImage::Format_RGBA8888);
    QImage mask(source.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    const QRgb highlightPaint = theme::clippingHighlights.rgba();
    const QRgb shadowPaint = theme::clippingShadows.rgba();
    for (int y = 0; y < source.height(); ++y) {
        const uchar* in = source.constScanLine(y);
        auto* out = reinterpret_cast<QRgb*>(mask.scanLine(y));
        for (int x = 0; x < source.width(); ++x, in += 4) {
            if (in[3] == 0) {
                continue;
            }
            const bool high = in[0] == 255 || in[1] == 255 || in[2] == 255;
            const bool low = in[0] == 0 || in[1] == 0 || in[2] == 0;
            if (highlights && high) {
                out[x] = highlightPaint;
            } else if (shadows && low) {
                out[x] = shadowPaint;
            }
        }
    }
    return mask;
}

} // namespace arraw::app
