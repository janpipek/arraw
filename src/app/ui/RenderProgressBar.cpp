#include "RenderProgressBar.h"

#include <QColor>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <cmath>

namespace arraw::app {

namespace {

/// Share of the track that the sweeping segment covers.
constexpr double sweepShare = 0.3;

/// Opacity of the track behind the fill, out of 255.
constexpr int trackAlpha = 48;

} // namespace

RenderProgressBar::RenderProgressBar(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(thickness);
    hide();
}

void RenderProgressBar::setDisplay(const RenderActivity::Display& display) {
    display_ = display;
    if (!display.visible) {
        hide();
        return;
    }
    if (isHidden()) {
        show();
        raise();
    }
    update();
}

std::pair<int, int> RenderProgressBar::fill(const RenderActivity::Display& display, int width) {
    if (!display.visible || width <= 0) {
        return {0, 0};
    }
    if (display.fraction) {
        return {0, static_cast<int>(std::lround(std::clamp(*display.fraction, 0.0, 1.0) * width))};
    }
    // The segment enters from the left edge and leaves by the right one, clipped to the track.
    const int length = static_cast<int>(std::lround(sweepShare * width));
    const int start = static_cast<int>(std::lround(display.sweep * (width + length))) - length;
    const int left = std::max(start, 0);
    const int right = std::min(start + length, width);
    return {left, std::max(right - left, 0)};
}

void RenderProgressBar::paintEvent(QPaintEvent* /*event*/) {
    if (!display_.visible) {
        return;
    }
    QPainter painter(this);
    const double ratio = devicePixelRatioF();
    // Painted in device pixels: undo the scale the painter carries, so every edge is a whole one.
    painter.scale(1.0 / ratio, 1.0 / ratio);
    const int width = static_cast<int>(std::lround(this->width() * ratio));
    const int height = static_cast<int>(std::lround(this->height() * ratio));
    QColor colour = palette().color(QPalette::Highlight);
    QColor track = colour;
    track.setAlpha(trackAlpha);
    painter.fillRect(QRect(0, 0, width, height), track);
    const auto [left, length] = fill(display_, width);
    if (length > 0) {
        painter.fillRect(QRect(left, 0, length, height), colour);
    }
}

} // namespace arraw::app
