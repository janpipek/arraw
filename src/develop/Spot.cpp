#include "develop/Spot.h"
#include "pipeline/ImagePipeline.h"

#include <algorithm>
#include <cmath>

float spotWeight(const Spot& spot, QPointF px) {
    if (spot.radius <= 0.0)
        return 0.0f;
    const double dx = px.x() - spot.destination.x();
    const double dy = px.y() - spot.destination.y();
    const double nd = std::sqrt(dx * dx + dy * dy) / spot.radius;
    const double inner = 1.0 - spot.feather;
    double s;
    if (spot.feather <= 0.0) {
        s = nd >= 1.0 ? 1.0 : 0.0;
    } else {
        const double t = std::clamp((nd - inner) / spot.feather, 0.0, 1.0);
        s = t * t * (3.0 - 2.0 * t);
    }
    return static_cast<float>(1.0 - s);
}

ImageBuffer applySpots(const ImageBuffer& buf, const std::vector<Spot>& spots) {
    if (spots.empty())
        return buf;

    ImageBuffer result = buf;

    for (const Spot& spot : spots) {
        const int r = static_cast<int>(std::ceil(spot.radius));
        const int x0 = std::max(0, static_cast<int>(spot.destination.x()) - r);
        const int y0 = std::max(0, static_cast<int>(spot.destination.y()) - r);
        const int x1 = std::min(buf.width - 1, static_cast<int>(spot.destination.x()) + r);
        const int y1 = std::min(buf.height - 1, static_cast<int>(spot.destination.y()) + r);

        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float w = spotWeight(spot, {static_cast<double>(x), static_cast<double>(y)});
                if (w <= 0.0f)
                    continue;

                // Sample the corresponding source pixel (same relative offset).
                const int sx = std::clamp(
                    static_cast<int>(std::round(x + (spot.source.x() - spot.destination.x()))),
                    0,
                    buf.width - 1);
                const int sy = std::clamp(
                    static_cast<int>(std::round(y + (spot.source.y() - spot.destination.y()))),
                    0,
                    buf.height - 1);

                const int di = (y * buf.width + x) * 3;
                const int si = (sy * buf.width + sx) * 3;

                for (int c = 0; c < 3; ++c)
                    result.data[static_cast<size_t>(di + c)]
                        = (1.0f - w) * buf.data[static_cast<size_t>(di + c)]
                          + w * buf.data[static_cast<size_t>(si + c)];
            }
        }
    }

    return result;
}

QPointF autoSourcePosition(QPointF destination, double offset, int bufW, int bufH) {
    const double x = std::clamp(destination.x() + offset, 0.0, static_cast<double>(bufW - 1));
    const double y = std::clamp(destination.y(), 0.0, static_cast<double>(bufH - 1));
    return {x, y};
}
