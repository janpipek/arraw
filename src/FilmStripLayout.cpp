#include "FilmStripLayout.h"
#include <QtMath>

namespace filmstrip {

int cellWidth(int contentHeight, QSize imageSize) {
    if (contentHeight <= 0)
        return 0;
    if (imageSize.width() <= 0 || imageSize.height() <= 0)
        return contentHeight; // square fallback for unknown/degenerate sizes
    return qRound(contentHeight * imageSize.width() / double(imageSize.height()));
}

} // namespace filmstrip
