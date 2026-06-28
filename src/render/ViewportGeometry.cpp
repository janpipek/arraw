#include "core/Orientation.h"
#include "render/ViewportGeometry.h"

#include <cmath>
#include <numbers>

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

QRectF shrinkInsideRotation(const QRectF& cropRect, float degrees, float aspect) {
    const auto inside = [&](const QRectF& r) {
        const QPointF corners[4] = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
        const float eps = 1e-4f;
        for (const QPointF& c : corners) {
            const QPointF s
                = rotateTextureUv(float(c.x()), float(c.y()), degrees, aspect, 0.5f, 0.5f);
            if (s.x() < -eps || s.x() > 1.0f + eps || s.y() < -eps || s.y() > 1.0f + eps)
                return false;
        }
        return true;
    };
    if (inside(cropRect))
        return cropRect; // already fits — shrink-only, never grow back

    // Scale about the centre, preserving the rect's aspect, and binary-search the
    // largest factor that fits. The rotation pivot is the image centre (0.5,0.5);
    // a crop centred off-pivot still shrinks toward its own centre.
    const QPointF centre = cropRect.center();
    const double halfW = cropRect.width() / 2.0;
    const double halfH = cropRect.height() / 2.0;
    const auto scaled = [&](double s) {
        return QRectF(
            centre.x() - halfW * s,
            centre.y() - halfH * s,
            cropRect.width() * s,
            cropRect.height() * s);
    };
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (inside(scaled(mid)))
            lo = mid;
        else
            hi = mid;
    }
    return scaled(lo);
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
    const QPointF rotUV = rotateTextureUv(fu, fv, rotation, imageAspect, 0.5f, 0.5f);
    // Orientation maps the oriented-frame UV to the native buffer, after the
    // rotation — exactly as image.vert does (docs/adr/0029).
    const QPointF bufUV = orient::orientedToBuffer(rotUV, orientation);
    return {bufUV.x() * originalSize.width(), bufUV.y() * originalSize.height()};
}

QPointF Geometry::bufferPixelToViewport(QPointF bufPx) const {
    if (!hasOriginalSize())
        return {};
    const float bu = float(bufPx.x()) / float(originalSize.width());
    const float bv = float(bufPx.y()) / float(originalSize.height());
    // Undo orientation first (native → oriented frame), then the rotation —
    // the exact inverse of viewportToBufferPixel / image.vert.
    const QPointF oriented = orient::bufferToOriented({bu, bv}, orientation);
    const QPointF fullUV = rotateTextureUv(
        float(oriented.x()), float(oriented.y()), -rotation, imageAspect, 0.5f, 0.5f);
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
