#include "FilmStripLayout.h"

namespace filmstrip {

int cellWidth(int contentHeight, QSize imageSize) {
    Q_UNUSED(imageSize)
    if (contentHeight <= 0)
        return 0;
    return contentHeight;
}

} // namespace filmstrip
