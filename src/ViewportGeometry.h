#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>

namespace viewport {

// CPU mirror of the rotation in image.vert (keep in sync). UV space is not
// square, so x is scaled by the image aspect before rotating to make the
// rotation isotropic in pixel space, then scaled back.
QPointF rotateTextureUv(float u, float v, float degrees, float aspect, float cx, float cy);

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

    bool hasOriginalSize() const;
    QPointF cropUvToViewport(float u, float v) const;
    QPointF viewportToCropUv(QPointF pos) const;
    QPointF viewportToBufferPixel(QPointF pos) const;
    QPointF bufferPixelToViewport(QPointF bufPx) const;
    double bufferRadiusToViewport(QPointF centerBufPx, double radius) const;
};

} // namespace viewport
