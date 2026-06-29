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

    const int w = out.width;
    const int h = out.height;
    const double flow = std::clamp(brush.flow, 0.0, 1.0);

    // Interpolate dab centres along the path so a dragged stroke leaves no gap.
    // Spacing a quarter of the radius keeps adjacent dabs well overlapped.
    const double spacing = std::max(0.5, brush.radius * 0.25);
    std::vector<QPointF> centres{path.front()};
    for (size_t i = 1; i < path.size(); ++i) {
        const QPointF a = path[i - 1];
        const QPointF b = path[i];
        const double segLen = std::hypot(b.x() - a.x(), b.y() - a.y());
        for (double d = spacing; d < segLen; d += spacing) {
            const double t = d / segLen;
            centres.push_back({a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t});
        }
        centres.push_back(b);
    }

    // Per-stroke coverage: the max dab profile (× flow) over every dab in the
    // stroke, so overlapping dabs within one stroke never exceed flow.
    std::vector<float> coverage(static_cast<size_t>(w) * h, 0.0f);
    const double r = brush.radius;
    for (const QPointF& c : centres) {
        const int x0 = std::max(0, static_cast<int>(std::floor(c.x() - r)));
        const int x1 = std::min(w - 1, static_cast<int>(std::ceil(c.x() + r)));
        const int y0 = std::max(0, static_cast<int>(std::floor(c.y() - r)));
        const int y1 = std::min(h - 1, static_cast<int>(std::ceil(c.y() + r)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const double dist = std::hypot(x - c.x(), y - c.y());
                const float cov = static_cast<float>(brushDabProfile(dist, r, brush.feather) * flow);
                float& acc = coverage[static_cast<size_t>(y) * w + x];
                acc = std::max(acc, cov);
            }
        }
    }

    // Composite the stroke onto base, clamped to [0,1]. Add: base + coverage;
    // Erase: base - coverage.
    for (size_t i = 0; i < out.data.size(); ++i) {
        const float baseW = out.data[i] / 255.0f;
        const float result
            = std::clamp(brush.erase ? baseW - coverage[i] : baseW + coverage[i], 0.0f, 1.0f);
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
