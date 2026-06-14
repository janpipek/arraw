#include "FilmStripLayout.h"
#include <QtMath>
#include <algorithm>

namespace filmstrip {

int cellWidth(int contentHeight, QSize imageSize) {
    if (contentHeight <= 0)
        return 0;
    if (imageSize.width() <= 0 || imageSize.height() <= 0)
        return contentHeight;  // square fallback for unknown/degenerate sizes
    return qRound(contentHeight * imageSize.width() / double(imageSize.height()));
}

int centerScrollOffset(int index, int cellPitch, int viewportWidth, int contentWidth) {
    const int itemCenter = index * cellPitch + cellPitch / 2;
    const int desired = itemCenter - viewportWidth / 2;
    const int maxOffset = std::max(0, contentWidth - viewportWidth);
    return std::clamp(desired, 0, maxOffset);
}

} // namespace filmstrip
