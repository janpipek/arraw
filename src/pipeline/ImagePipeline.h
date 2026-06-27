#pragma once
#include "core/ImageBuffer.h"
#include "core/Orientation.h"
#include <array>
#include <vector>
#include <QPointF>
#include <QRectF>
#include <QSize>

// Box-filter 2× downsample (half W, half H). Safe to call off the main thread.
ImageBuffer downsample2x(const ImageBuffer& src);

// Scale a linear buffer so its near-max luma (99.5th percentile) lands at a fixed
// target, giving every photo a comparable starting exposure. Gain is clamped to
// [0.5, 4.0] so a bad estimate can't blow out or crush the image. Applied to both
// the demosaiced full-res and the embedded preview, so the two match on load.
void normalizeExposure(ImageBuffer& buf);

// Compute a 256-entry output LUT from tone-curve control points (Fritsch-Carlson monotone spline).
std::array<float, 256> computeCurveLUT(const std::vector<QPointF>& pts);

// Pixel size of a developed thumbnail: the source cropped to crop, scaled down so
// its longer edge is at most maxEdge (never upscaled), aspect preserved.
// Output pixel size of a developed thumbnail. `srcW`/`srcH` are the *native*
// buffer dims; an odd quarter-turn Orientation swaps them so the thumbnail keeps
// the oriented aspect (docs/adr/0028) — otherwise turned shots come out squished.
QSize developedThumbSize(
    int srcW, int srcH, const QRectF& crop, int maxEdge, orient::Orientation orientation = {});
