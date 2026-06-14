#pragma once
#include <QSize>

// Pure geometry for the horizontal filmstrip. No widget state — kept here so it
// can be unit-tested without constructing the view.
namespace filmstrip {

// Width of a cell that shows an image of `imageSize` at the strip's content
// height, preserving aspect ratio. Degenerate sizes fall back to a square cell.
int cellWidth(int contentHeight, QSize imageSize);

} // namespace filmstrip
