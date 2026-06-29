#include "develop/BrushMask.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>

#include <algorithm>
#include <cmath>

float brushDabProfile(double dist, double radius, double feather) {
    if (radius <= 0.0)
        return 0.0f;
    if (dist >= radius)
        return 0.0f;
    const double inner = radius * (1.0 - std::clamp(feather, 0.0, 1.0));
    if (dist <= inner)
        return 1.0f;
    // smoothstep from full coverage at `inner` down to 0 at `radius`.
    const double t = (dist - inner) / (radius - inner);
    return static_cast<float>(1.0 - t * t * (3.0 - 2.0 * t));
}

BrushRaster stampStroke(
    const BrushRaster& base, std::span<const QPointF> path, const BrushDab& brush) {
    BrushRaster out = base;
    if (path.empty() || brush.radius <= 0.0)
        return out;

    // Build the whole stroke's coverage one segment at a time, then composite.
    std::vector<float> coverage(static_cast<size_t>(out.width) * out.height, 0.0f);
    if (path.size() == 1) {
        stampSegmentCoverage(coverage, out.width, out.height, path.front(), path.front(), brush);
    } else {
        for (size_t i = 1; i < path.size(); ++i)
            stampSegmentCoverage(coverage, out.width, out.height, path[i - 1], path[i], brush);
    }
    return compositeStroke(base, coverage, brush.erase);
}

void stampSegmentCoverage(
    std::span<float> coverage, int width, int height, QPointF a, QPointF b, const BrushDab& brush) {
    if (brush.radius <= 0.0)
        return;
    const double flow = std::clamp(brush.flow, 0.0, 1.0);
    const double r = brush.radius;

    // Stamp one dab at `c` into `coverage` by max(), restricted to its bbox.
    auto stampAt = [&](QPointF c) {
        const int x0 = std::max(0, static_cast<int>(std::floor(c.x() - r)));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(c.x() + r)));
        const int y0 = std::max(0, static_cast<int>(std::floor(c.y() - r)));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(c.y() + r)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const double dist = std::hypot(x - c.x(), y - c.y());
                const float cov = static_cast<float>(brushDabProfile(dist, r, brush.feather) * flow);
                float& acc = coverage[static_cast<size_t>(y) * width + x];
                acc = std::max(acc, cov);
            }
        }
    };

    // Dab centres interpolated along [a,b] at a quarter-radius spacing so the
    // stroke leaves no gap; both endpoints are always stamped.
    const double spacing = std::max(0.5, r * 0.25);
    const double segLen = std::hypot(b.x() - a.x(), b.y() - a.y());
    stampAt(a);
    for (double d = spacing; d < segLen; d += spacing) {
        const double t = d / segLen;
        stampAt({a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t});
    }
    stampAt(b);
}

BrushRaster compositeStroke(const BrushRaster& base, std::span<const float> coverage, bool erase) {
    BrushRaster out = base;
    const size_t n = std::min(out.data.size(), coverage.size());
    for (size_t i = 0; i < n; ++i) {
        const float baseW = out.data[i] / 255.0f;
        const float result
            = std::clamp(erase ? baseW - coverage[i] : baseW + coverage[i], 0.0f, 1.0f);
        out.data[i] = static_cast<uint8_t>(std::lround(result * 255.0f));
    }
    return out;
}

QString encodeBrushRaster(const BrushRaster& raster) {
    if (raster.width <= 0 || raster.height <= 0
        || raster.data.size() != static_cast<size_t>(raster.width) * raster.height)
        return {};

    QImage img(raster.width, raster.height, QImage::Format_Grayscale8);
    for (int y = 0; y < raster.height; ++y)
        std::copy_n(
            raster.data.data() + static_cast<size_t>(y) * raster.width,
            raster.width,
            img.scanLine(y));

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!img.save(&buffer, "PNG"))
        return {};
    return QString::fromLatin1(png.toBase64());
}

BrushRaster decodeBrushRaster(const QString& base64Png) {
    const QByteArray png = QByteArray::fromBase64(base64Png.toLatin1());
    QImage img;
    if (!img.loadFromData(png, "PNG"))
        return {};
    img = img.convertToFormat(QImage::Format_Grayscale8);

    BrushRaster raster{
        img.width(),
        img.height(),
        std::vector<uint8_t>(static_cast<size_t>(img.width()) * img.height(), 0)};
    for (int y = 0; y < raster.height; ++y)
        std::copy_n(
            img.constScanLine(y),
            raster.width,
            raster.data.data() + static_cast<size_t>(y) * raster.width);
    return raster;
}
