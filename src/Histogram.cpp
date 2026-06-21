#include "Histogram.h"
#include <algorithm>
#include <cmath>
#include <QPainter>

// Maps a linear sRGB value in [0, 1] to a histogram bin [0, 255] using a
// precomputed piecewise sRGB gamma LUT — avoids per-pixel pow() calls.
static int srgbBin(float v) {
    static const auto lut = [] {
        std::array<uint8_t, 4096> t{};
        for (int i = 0; i < 4096; ++i) {
            float u = float(i) / 4095.0f;
            float enc = (u <= 0.0031308f) ? u * 12.92f : 1.055f * std::pow(u, 1.0f / 2.4f) - 0.055f;
            t[i] = uint8_t(int(enc * 255.0f + 0.5f));
        }
        return t;
    }();
    return lut[int(v * 4095.0f + 0.5f)];
}

Histogram::Histogram(QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(80);
    setMaximumHeight(120);
}

void Histogram::setSample(const QImage& img) {
    r.fill(0);
    g.fill(0);
    b.fill(0);
    rOver = gOver = bOver = 0;
    rUnder = gUnder = bUnder = 0;

    if (img.format() == QImage::Format_RGBX32FPx4) {
        // Pre-clamp sRGB-linear floats from the histoRaw shader pass.
        for (int y = 0; y < img.height(); ++y) {
            const float* line = reinterpret_cast<const float*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const float rv = line[x * 4 + 0];
                const float gv = line[x * 4 + 1];
                const float bv = line[x * 4 + 2];
                if (rv < 0.0f)
                    ++rUnder;
                else if (rv > 1.0f)
                    ++rOver;
                else
                    ++r[srgbBin(rv)];
                if (gv < 0.0f)
                    ++gUnder;
                else if (gv > 1.0f)
                    ++gOver;
                else
                    ++g[srgbBin(gv)];
                if (bv < 0.0f)
                    ++bUnder;
                else if (bv > 1.0f)
                    ++bOver;
                else
                    ++b[srgbBin(bv)];
            }
        }
    } else {
        const QImage sample = img.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < sample.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
            for (int x = 0; x < sample.width(); ++x) {
                ++r[qRed(line[x])];
                ++g[qGreen(line[x])];
                ++b[qBlue(line[x])];
            }
        }
    }

    peak = 1;
    for (int i = 0; i < kBins; ++i)
        peak = std::max({peak, r[i], g[i], b[i]});
    peak = std::max({peak, rOver, gOver, bOver, rUnder, gUnder, bUnder});

    update();
}

void Histogram::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));

    // Log-scale the bar heights so sparse bins stay visible next to the peak.
    const float yScale = float(height()) / std::log1p(float(peak));

    // Layout: [overflowColW | gap | main bins | gap | overflowColW]
    const int overflowColW = 8;
    const int gap = 2;
    const int mainW = width() - 2 * (overflowColW + gap);
    const int mainX = overflowColW + gap;
    const float xScale = float(mainW) / kBins;

    auto drawBins = [&](const std::array<int, kBins>& ch, QColor col) {
        col.setAlpha(160);
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        for (int i = 0; i < kBins; ++i) {
            float h = std::log1p(float(ch[i])) * yScale;
            p.drawRect(QRectF(mainX + i * xScale, height() - h, xScale, h));
        }
    };

    auto drawOverflow = [&](int count, float x, float w, QColor col) {
        if (count == 0)
            return;
        col.setAlpha(200);
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        float h = std::log1p(float(count)) * yScale;
        p.drawRect(QRectF(x, height() - h, w, h));
    };

    drawBins(r, QColor(220, 60, 60));
    drawBins(g, QColor(60, 180, 60));
    drawBins(b, QColor(60, 100, 220));

    // Underflow columns (left) — per-channel, stacked in the same narrow column.
    const float underX = 0.0f;
    drawOverflow(rUnder, underX, overflowColW, QColor(220, 60, 60));
    drawOverflow(gUnder, underX, overflowColW, QColor(60, 180, 60));
    drawOverflow(bUnder, underX, overflowColW, QColor(60, 100, 220));

    // Overflow columns (right) — per-channel.
    const float overX = float(mainX + mainW + gap);
    drawOverflow(rOver, overX, overflowColW, QColor(220, 60, 60));
    drawOverflow(gOver, overX, overflowColW, QColor(60, 180, 60));
    drawOverflow(bOver, overX, overflowColW, QColor(60, 100, 220));
}
