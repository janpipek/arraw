#include "Histogram.h"
#include <QPainter>
#include <algorithm>
#include <cmath>

Histogram::Histogram(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(80);
    setMaximumHeight(120);
}

void Histogram::setImage(const ImageBuffer& buf) {
    r.fill(0); g.fill(0); b.fill(0);

    const int pixels = buf.width * buf.height;
    for (int i = 0; i < pixels; ++i) {
        auto bin = [](float v) { return std::clamp(int(v * kBins), 0, kBins - 1); };
        ++r[bin(buf.data[i * 3 + 0])];
        ++g[bin(buf.data[i * 3 + 1])];
        ++b[bin(buf.data[i * 3 + 2])];
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
