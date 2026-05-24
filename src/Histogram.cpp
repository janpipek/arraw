#include "Histogram.h"
#include <QPainter>
#include <algorithm>
#include <cmath>

Histogram::Histogram(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(80);
    setMaximumHeight(120);
}

void Histogram::setImage(const ImageBuffer& buf) {
    rawBuf = buf;
    recompute();
}

void Histogram::setAdjustments(const AdjustmentParams& p) {
    params = p;
    if (rawBuf.valid())
        recompute();
}

// CPU mirror of the shader pipeline — applied to sampled preview pixels.
static float applyExposure(float v, float ev) {
    return v * std::pow(2.0f, ev);
}

static float applyContrast(float v, float contrast) {
    if (std::abs(contrast) < 0.001f) return v;
    return (v - 0.5f) * (1.0f + contrast * 0.008f) + 0.5f;
}

static float applyHighlights(float v, float hl) {
    if (std::abs(hl) < 0.001f) return v;
    float mask = std::max(0.0f, std::min(1.0f, (v - 0.5f) * 2.0f));
    return v + hl * 0.005f * mask;
}

static float applyShadows(float v, float sh) {
    if (std::abs(sh) < 0.001f) return v;
    float mask = std::max(0.0f, std::min(1.0f, 1.0f - v * 2.0f));
    return v + sh * 0.005f * mask;
}

static float gamma(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return std::pow(v, 1.0f / 2.2f);
}

void Histogram::recompute() {
    r.fill(0); g.fill(0); b.fill(0);

    const int pixels = rawBuf.width * rawBuf.height;
    const auto& p = params;

    for (int i = 0; i < pixels; i += kStride) {
        float rv = rawBuf.data[i * 3 + 0];
        float gv = rawBuf.data[i * 3 + 1];
        float bv = rawBuf.data[i * 3 + 2];

        // Exposure
        rv = applyExposure(rv, p.exposure);
        gv = applyExposure(gv, p.exposure);
        bv = applyExposure(bv, p.exposure);

        // Contrast
        rv = applyContrast(rv, p.contrast);
        gv = applyContrast(gv, p.contrast);
        bv = applyContrast(bv, p.contrast);

        // Highlights / Shadows (luma-based mask for speed)
        rv = applyHighlights(rv, p.highlights);
        gv = applyHighlights(gv, p.highlights);
        bv = applyHighlights(bv, p.highlights);
        rv = applyShadows(rv, p.shadows);
        gv = applyShadows(gv, p.shadows);
        bv = applyShadows(bv, p.shadows);

        // Temperature (Kelvin, normalised around 5500K)
        float t = (p.temperature - 5500.0f) / 5500.0f;
        rv += t * 0.15f;
        bv -= t * 0.15f;

        // Saturation
        if (std::abs(p.saturation) > 0.001f) {
            float luma = 0.2126f * rv + 0.7152f * gv + 0.0722f * bv;
            float sat  = 1.0f + p.saturation / 100.0f;
            rv = luma + (rv - luma) * sat;
            gv = luma + (gv - luma) * sat;
            bv = luma + (bv - luma) * sat;
        }

        // Gamma → display space
        auto bin = [](float v) {
            return std::clamp(int(gamma(v) * kBins), 0, kBins - 1);
        };
        ++r[bin(rv)];
        ++g[bin(gv)];
        ++b[bin(bv)];
    }

    peak = 1;
    for (int i = 0; i < kBins; ++i)
        peak = std::max({peak, r[i], g[i], b[i]});

    update();
}

void Histogram::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));

    const float xScale = float(width())  / kBins;
    const float yScale = float(height()) / std::log1p(float(peak));

    auto draw = [&](const std::array<int, kBins>& ch, QColor col) {
        col.setAlpha(160);
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        for (int i = 0; i < kBins; ++i) {
            float h = std::log1p(float(ch[i])) * yScale;
            p.drawRect(QRectF(i * xScale, height() - h, xScale, h));
        }
    };

    draw(r, QColor(220, 60,  60));
    draw(g, QColor(60,  180, 60));
    draw(b, QColor(60,  100, 220));
}
