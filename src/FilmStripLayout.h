#pragma once
#include <QSize>

// Pure geometry for the horizontal filmstrip. No widget state — kept here so it
// can be unit-tested without constructing the view.
namespace filmstrip {

// Side length of a square cell at the strip's content height. The image size is
// accepted so callers can keep the same path before thumbnails are loaded.
int cellWidth(int contentHeight, QSize imageSize);

} // namespace filmstrip
