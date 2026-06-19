#include "ViewportGeometry.h"

#include <numbers>
#include <cmath>

namespace viewport {

QPointF rotateTextureUv(float u, float v, float degrees, float aspect, float cx, float cy) {
    float dx = (u - cx) * aspect;
    float dy = v - cy;
    const float rad = degrees * float(std::numbers::pi) / 180.0f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float rx = c * dx - s * dy;
    const float ry = s * dx + c * dy;
    return {rx / aspect + cx, ry + cy};
}

bool Geometry::hasOriginalSize() const {
    return originalSize.width() > 0 && originalSize.height() > 0;
}

QPointF Geometry::cropUvToViewport(float u, float v) const {
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0)
        return {};

    const float viewportAspect = float(viewportSize.width()) / float(viewportSize.height());
    const float sx = zoom * (displayAspect / viewportAspect);
    const float sy = zoom;
    const float ndcX = (u * 2.0f - 1.0f) * sx + float(pan.x());
    const float ndcY = (1.0f - 2.0f * v) * sy + float(pan.y());
    return {(ndcX + 1.0f) * viewportSize.width() / 2.0f, (1.0f - ndcY) * viewportSize.height() / 2.0f};
}

QPointF Geometry::viewportToCropUv(QPointF pos) const {
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0)
        return {};

    const float viewportAspect = float(viewportSize.width()) / float(viewportSize.height());
    const float sx = zoom * (displayAspect / viewportAspect);
    const float sy = zoom;
    const float ndcX = float(pos.x()) * 2.0f / viewportSize.width() - 1.0f;
    const float ndcY = 1.0f - float(pos.y()) * 2.0f / viewportSize.height();
    const float u = ((ndcX - float(pan.x())) / sx + 1.0f) / 2.0f;
    const float v = (1.0f - (ndcY - float(pan.y())) / sy) / 2.0f;
    return {u, v};
}

QPointF Geometry::viewportToBufferPixel(QPointF pos) const {
    if (!hasOriginalSize())
        return {};
    const QPointF cropUV = viewportToCropUv(pos);
    const float fu = float(cropRect.left() + cropUV.x() * cropRect.width());
    const float fv = float(cropRect.top() + cropUV.y() * cropRect.height());
    const QPointF bufUV = rotateTextureUv(fu, fv, rotation, imageAspect, 0.5f, 0.5f);
    return {bufUV.x() * originalSize.width(), bufUV.y() * originalSize.height()};
}

QPointF Geometry::bufferPixelToViewport(QPointF bufPx) const {
    if (!hasOriginalSize())
        return {};
    const float bu = float(bufPx.x()) / float(originalSize.width());
    const float bv = float(bufPx.y()) / float(originalSize.height());
    const QPointF fullUV = rotateTextureUv(bu, bv, -rotation, imageAspect, 0.5f, 0.5f);
    const float cu = float((fullUV.x() - cropRect.left()) / cropRect.width());
    const float cv = float((fullUV.y() - cropRect.top()) / cropRect.height());
    return cropUvToViewport(cu, cv);
}

double Geometry::bufferRadiusToViewport(QPointF centerBufPx, double radius) const {
    const QPointF a = bufferPixelToViewport(centerBufPx);
    const QPointF b = bufferPixelToViewport({centerBufPx.x(), centerBufPx.y() + radius});
    const QPointF d = b - a;
    return std::sqrt(QPointF::dotProduct(d, d));
}

} // namespace viewport
