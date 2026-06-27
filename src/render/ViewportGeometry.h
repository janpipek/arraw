#pragma once

#include "core/Orientation.h"

#include <QPointF>
#include <QRectF>
#include <QSize>

namespace viewport {

// CPU mirror of the rotation in image.vert (keep in sync). UV space is not
// square, so x is scaled by the image aspect before rotating to make the
// rotation isotropic in pixel space, then scaled back.
QPointF rotateTextureUv(float u, float v, float degrees, float aspect, float cx, float cy);

// Shrink a normalised crop rectangle about its own centre — preserving its UV
// aspect ratio — just enough that all four corners, after the fine ±45° Rotation
// (mirrored from image.vert via rotateTextureUv), land inside the real image
// [0,1]². This is the auto-refit applied when a Straighten exposes the rotation's
// empty corners (CONTEXT.md, docs/adr/0028). Shrink-only: a crop already inside
// is returned unchanged — it never grows back. `aspect` is the image width/height.
QRectF shrinkInsideRotation(const QRectF& cropRect, float degrees, float aspect);

// Pure geometry used by ImageViewport tool overlays and hit-tests. It has no Qt
// widget or renderer dependency, so crop/mask/spot tools can share one mapping
// contract and exercise it directly in tests.
struct Geometry {
    QSize viewportSize;
    QSize originalSize;
    float imageAspect = 1.0f;
    float displayAspect = 1.0f;
    float zoom = 1.0f;
    QPointF pan = {0, 0};
    QRectF cropRect = {0, 0, 1, 1};
    float rotation = 0.0f;
    // imageAspect and displayAspect are the *oriented* aspects (W/H swapped when
    // the orientation is an odd quarter-turn); originalSize stays the *native*
    // buffer pixel size. Orientation is applied after the rotation, matching the
    // shader (docs/adr/0028), so buffer↔viewport stays in lock-step with the GPU.
    orient::Orientation orientation;

    bool hasOriginalSize() const;
    QPointF cropUvToViewport(float u, float v) const;
    QPointF viewportToCropUv(QPointF pos) const;
    QPointF viewportToBufferPixel(QPointF pos) const;
    QPointF bufferPixelToViewport(QPointF bufPx) const;
    double bufferRadiusToViewport(QPointF centerBufPx, double radius) const;
};

} // namespace viewport
