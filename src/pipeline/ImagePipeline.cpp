#include "pipeline/ImagePipeline.h"
#include "core/Orientation.h"
#include "core/WorkingSpace.h"
#include <algorithm>
#include <cmath>

std::array<float, 256> computeCurveLUT(const std::vector<QPointF>& ctrl) {
    std::array<float, 256> lut{};

    auto pts = ctrl;
    std::sort(pts.begin(), pts.end(), [](const QPointF& a, const QPointF& b) {
        return a.x() < b.x();
    });
    pts.erase(
        std::unique(
            pts.begin(),
            pts.end(),
            [](const QPointF& a, const QPointF& b) { return std::abs(a.x() - b.x()) < 1e-6; }),
        pts.end());

    const int m = int(pts.size());
    if (m == 0) {
        lut.fill(0.0f);
        return lut;
    }
    if (m == 1) {
        lut.fill(float(pts[0].y()));
        return lut;
    }

    // Fritsch-Carlson monotone cubic spline
    std::vector<double> delta(m - 1);
    for (int i = 0; i < m - 1; ++i) {
        const double dx = pts[i + 1].x() - pts[i].x();
        delta[i] = dx > 1e-10 ? (pts[i + 1].y() - pts[i].y()) / dx : 0.0;
    }

    std::vector<double> mk(m);
    mk[0] = delta[0];
    mk[m - 1] = delta[m - 2];
    for (int i = 1; i < m - 1; ++i)
        mk[i] = (delta[i - 1] + delta[i]) * 0.5;

    // Fritsch-Carlson condition: the curve stays monotone between two control
    // points iff the normalised tangents satisfy a² + b² ≤ 9; rescale any
    // tangent pair outside that circle (prevents overshoot/ringing).
    for (int i = 0; i < m - 1; ++i) {
        if (std::abs(delta[i]) < 1e-10) {
            mk[i] = mk[i + 1] = 0.0;
            continue;
        }
        const double a = mk[i] / delta[i];
        const double b = mk[i + 1] / delta[i];
        const double h = a * a + b * b;
        if (h > 9.0) {
            const double s = 3.0 / std::sqrt(h);
            mk[i] = a * s * delta[i];
            mk[i + 1] = b * s * delta[i];
        }
    }

    for (int k = 0; k < 256; ++k) {
        const double x = k / 255.0;
        if (x <= pts[0].x()) {
            lut[k] = float(std::clamp(pts[0].y(), 0.0, 1.0));
            continue;
        }
        if (x >= pts[m - 1].x()) {
            lut[k] = float(std::clamp(pts[m - 1].y(), 0.0, 1.0));
            continue;
        }

        int i = 0;
        while (i < m - 2 && x > pts[i + 1].x())
            ++i;

        const double h2 = pts[i + 1].x() - pts[i].x();
        const double t = (x - pts[i].x()) / h2;
        const double t2 = t * t, t3 = t2 * t;
        const double y = (2 * t3 - 3 * t2 + 1) * pts[i].y() + (t3 - 2 * t2 + t) * h2 * mk[i]
                         + (-2 * t3 + 3 * t2) * pts[i + 1].y() + (t3 - t2) * h2 * mk[i + 1];
        lut[k] = float(std::clamp(y, 0.0, 1.0));
    }
    return lut;
}

ImageBuffer downsample2x(const ImageBuffer& src) {
    if (!src.valid())
        return {};

    ImageBuffer dst;
    dst.width = src.width / 2;
    dst.height = src.height / 2;
    dst.data.resize(dst.width * dst.height * 3, 0.0f);

    const int sw = src.width;
    for (int y = 0; y < dst.height; ++y) {
        for (int x = 0; x < dst.width; ++x) {
            const int s0 = ((y * 2) * sw + (x * 2)) * 3;
            const int s1 = ((y * 2) * sw + (x * 2 + 1)) * 3;
            const int s2 = ((y * 2 + 1) * sw + (x * 2)) * 3;
            const int s3 = ((y * 2 + 1) * sw + (x * 2 + 1)) * 3;
            const int d = (y * dst.width + x) * 3;
            for (int c = 0; c < 3; ++c)
                dst.data[d + c] = (src.data[s0 + c] + src.data[s1 + c] + src.data[s2 + c]
                                   + src.data[s3 + c])
                                  * 0.25f;
        }
    }
    return dst;
}

QSize developedThumbSize(
    int srcW, int srcH, const QRectF& crop, int maxEdge, orient::Orientation orientation) {
    // An odd quarter-turn presents the buffer with width and height swapped, and
    // the crop is normalised in that oriented frame (docs/adr/0029).
    if (orient::swapsAspect(orientation))
        std::swap(srcW, srcH);
    const int cw = std::max(1, int(std::lround(srcW * crop.width())));
    const int ch = std::max(1, int(std::lround(srcH * crop.height())));

    const int longEdge = std::max(cw, ch);
    if (longEdge <= maxEdge)
        return {cw, ch}; // never upscale

    const double s = double(maxEdge) / double(longEdge);
    return {std::max(1, int(std::lround(cw * s))), std::max(1, int(std::lround(ch * s)))};
}

void normalizeExposure(ImageBuffer& buf) {
    if (!buf.valid())
        return;

    std::vector<float> luma;
    luma.reserve(size_t(buf.width * buf.height / 16 + 1));

    constexpr int kStride = 16;

    const int pixels = buf.width * buf.height;
    for (int i = 0; i < pixels; i += kStride) {
        const float* p = buf.data.data() + i * 3;
        const float y = p[0] * kLumaR + p[1] * kLumaG + p[2] * kLumaB;
        if (std::isfinite(y) && y > 0.0f)
            luma.push_back(y);
    }

    if (luma.empty())
        return;

    const size_t idx = std::min(luma.size() - 1, size_t(luma.size() * 0.995f));
    std::nth_element(luma.begin(), luma.begin() + idx, luma.end());

    const float highlight = luma[idx];
    if (highlight <= 0.0f)
        return;

    constexpr float kTargetHighlight = 0.78f;
    const float gain = std::clamp(kTargetHighlight / highlight, 0.5f, 4.0f);
    for (float& v : buf.data)
        v = std::max(0.0f, v * gain);
}
