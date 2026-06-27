#include "core/Orientation.h"
#include "pipeline/ImagePipeline.h"
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
    // the crop is normalised in that oriented frame (docs/adr/0028).
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

// ── White balance (docs/adr/0025) ───────────────────────────────────────────
// A per-channel multiplicative gain in linear Rec.2020, so a black pixel
// (c == 0) stays black for any setting. The gain is blackbody-derived: a target
// Kelvin maps through its Planckian-locus chromaticity to a linear Rec.2020
// colour, and the gain is the reference (5500 K) colour over the target's,
// normalised so green == 1 (temperature moves R/B about the green anchor).
// Tint is the orthogonal green↔magenta axis, applied as a green gain.
namespace {

// Correlated-colour-temperature → CIE 1931 xy on the Planckian locus
// (Kim et al. cubic-spline approximation, valid ~1667..25000 K).
void planckianXY(float kelvin, double& x, double& y) {
    const double T = std::clamp(double(kelvin), 1667.0, 25000.0);
    const double T2 = T * T, T3 = T2 * T;
    if (T < 4000.0)
        x = -0.2661239e9 / T3 - 0.2343589e6 / T2 + 0.8776956e3 / T + 0.179910;
    else
        x = -3.0258469e9 / T3 + 2.1070379e6 / T2 + 0.2226347e3 / T + 0.240390;
    const double x2 = x * x, x3 = x2 * x;
    if (T < 2222.0)
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    else if (T < 4000.0)
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    else
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
}

// CIE xy (luminance Y = 1) → linear Rec.2020 RGB (XYZ→Rec.2020, D65).
std::array<double, 3> xyToLinearRec2020(double x, double y) {
    const double Y = 1.0;
    const double X = (y > 1e-9) ? x * Y / y : 0.0;
    const double Z = (y > 1e-9) ? (1.0 - x - y) * Y / y : 0.0;
    return {
        1.7166511880 * X - 0.3556707838 * Y - 0.2533662814 * Z,
        -0.6666843518 * X + 1.6164812366 * Y + 0.0157685458 * Z,
        0.0176398574 * X - 0.0427706133 * Y + 0.9421031212 * Z,
    };
}

constexpr float kNeutralKelvin = 5500.0f;
// Tint strength: full ±100 slider is a ±2^(0.2) ≈ ±15% green gain.
constexpr double kTintGainExp = 0.2;

std::array<double, 3> illuminantRec2020(float kelvin) {
    double x, y;
    planckianXY(kelvin, x, y);
    return xyToLinearRec2020(x, y);
}

} // namespace

std::array<float, 3> whiteBalanceGain(float kelvin, float tint) {
    static const std::array<double, 3> ref = illuminantRec2020(kNeutralKelvin);
    const std::array<double, 3> w = illuminantRec2020(kelvin);

    // von Kries gain = reference illuminant / target illuminant, per channel.
    auto safe = [](double v) { return std::max(v, 1e-4); };
    std::array<double, 3> g = {
        safe(ref[0]) / safe(w[0]),
        safe(ref[1]) / safe(w[1]),
        safe(ref[2]) / safe(w[2]),
    };
    // Normalise so green == 1: temperature moves R/B about the green anchor.
    const double gn = g[1];
    g[0] /= gn;
    g[1] = 1.0;
    g[2] /= gn;

    // Tint: orthogonal green↔magenta gain (+ = greener, matching the prior model).
    g[1] *= std::pow(2.0, kTintGainExp * double(tint) / 100.0);

    return {float(g[0]), float(g[1]), float(g[2])};
}

void whiteBalanceFromNeutral(float r, float g, float b, float& kelvin, float& tint) {
    // Find the Kelvin whose gain neutralises the R/B balance (r*kr == b*kb), i.e.
    // kr(K)/kb(K) == b/r. The ratio is monotonic in K — bisect over the range.
    const double wantRatio = (r > 1e-6) ? double(b) / double(r) : 1.0;
    double lo = 2000.0, hi = 12000.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        const auto gm = whiteBalanceGain(float(mid), 0.0f);
        const double ratio = double(gm[0]) / double(gm[2]); // kr/kb
        if (ratio < wantRatio)
            lo = mid;
        else
            hi = mid;
    }
    kelvin = float(std::clamp(0.5 * (lo + hi), 2000.0, 12000.0));

    // Tint brings green onto the (now equal) R/B level: g * tintGain == r*kr.
    const auto gk = whiteBalanceGain(kelvin, 0.0f);
    const double m = 0.5 * (double(r) * gk[0] + double(b) * gk[2]); // balanced R/B level
    const double tintGain = (g > 1e-6) ? m / double(g) : 1.0;
    tint = float(
        std::clamp(100.0 * std::log2(std::max(tintGain, 1e-6)) / kTintGainExp, -100.0, 100.0));
}
