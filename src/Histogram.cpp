#include "Histogram.h"
#include <algorithm>
#include <cmath>
#include <QPainter>

Histogram::Histogram(QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(80);
    setMaximumHeight(120);
}

void Histogram::setSample(const QImage& img) {
    r.fill(0);
    g.fill(0);
    b.fill(0);

    const QImage sample = img.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < sample.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
        for (int x = 0; x < sample.width(); ++x) {
            ++r[qRed(line[x])];
            ++g[qGreen(line[x])];
            ++b[qBlue(line[x])];
        }
    }

    peak = 1;
    for (int i = 0; i < kBins; ++i)
        peak = std::max({peak, r[i], g[i], b[i]});

    update();
}

void Histogram::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));

    // Log-scale the bar heights so sparse bins stay visible next to the peak.
    const float xScale = float(width()) / kBins;
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

    draw(r, QColor(220, 60, 60));
    draw(g, QColor(60, 180, 60));
    draw(b, QColor(60, 100, 220));
}
