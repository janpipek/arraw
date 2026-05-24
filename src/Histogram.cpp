#include "Histogram.h"
#include <QPainter>
#include <algorithm>
#include <cmath>

Histogram::Histogram(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(80);
    setMaximumHeight(120);

    debounce.setSingleShot(true);
    debounce.setInterval(150);
    connect(&debounce, &QTimer::timeout, this, &Histogram::recompute);
}

void Histogram::setImage(const ImageBuffer& buf) {
    rawBuf = buf;
    recompute();
}

void Histogram::setAdjustments(const AdjustmentParams& p) {
    params = p;
    if (rawBuf.valid())
        debounce.start();   // restarts the timer if already running
}

// CPU mirror of the shader pipeline — applied to sampled preview pixels.
static float applyExposure(float v, float ev) {
    return v * std::pow(2.0f, ev);
}

static float applyContrast(float v, float contrast) {
    if (std::abs(contrast) < 0.001f) return v;
    return (v - 0.5f) * (1.0f + contrast / kToneSliderToUniform * 0.8f) + 0.5f;
}

static float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static void applyToneRegions(float& r, float& g, float& b, const AdjustmentParams& p) {
    if (std::abs(p.highlights) < 0.001f && std::abs(p.shadows) < 0.001f &&
        std::abs(p.whites) < 0.001f && std::abs(p.blacks) < 0.001f)
        return;

    constexpr float kLumaR = 0.2126f, kLumaG = 0.7152f, kLumaB = 0.0722f;
    const float y = kLumaR * r + kLumaG * g + kLumaB * b;

    const float hl = smoothstep(0.28f, 0.88f, y);
    const float sh = 1.0f - smoothstep(0.12f, 0.78f, y);
    const float w  = smoothstep(0.52f, 0.97f, y);
    const float blk = 1.0f - smoothstep(0.03f, 0.48f, y);

    const float delta = p.highlights / kToneSliderToUniform * 0.5f * hl
                      + p.shadows    / kToneSliderToUniform * 0.5f * sh
                      + p.whites     / kToneSliderToUniform * 0.25f * w
                      + p.blacks     / kToneSliderToUniform * 0.25f * blk;

    const float y2 = std::max(y + delta, 0.0f);
    const float scale = y2 / std::max(y, 1e-5f);
    r *= scale;
    g *= scale;
    b *= scale;
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

        applyToneRegions(rv, gv, bv, p);

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
