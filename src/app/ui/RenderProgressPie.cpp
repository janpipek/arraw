#include "RenderProgressPie.h"

#include "ThemeColors.h"

#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace arraw::app {

namespace {

/// Margin around the pie, in pixels.
constexpr int margin = 2;

/// @brief Gives the fraction in steps of the resolution, or none for a missing fraction.
int stepsOf(const RenderActivity::Display& display) {
    if (!display.visible) {
        return RenderProgressPie::resolution;
    }
    if (!display.fraction) {
        return 0;
    }
    return static_cast<int>(
        std::lround(std::clamp(*display.fraction, 0.0, 1.0) * RenderProgressPie::resolution));
}

} // namespace

RenderProgressPie::RenderProgressPie(QWidget* parent) : QWidget(parent) {
    setObjectName("renderProgressPie");
    setAccessibleName(tr("Render progress"));
    setAccessibleDescription(describe());
    setToolTip(describe());
}

double RenderProgressPie::filled() const noexcept {
    return static_cast<double>(stepsOf(display_)) / resolution;
}

QString RenderProgressPie::describe() const {
    if (!display_.visible) {
        return tr("Up to date");
    }
    QString text = renderStepText(display_.step);
    if (display_.fraction) {
        text += QStringLiteral(" %1%").arg(std::lround(filled() * 100));
    }
    return text;
}

QSize RenderProgressPie::sizeHint() const {
    const int side = fontMetrics().height();
    return {side, side};
}

QSize RenderProgressPie::minimumSizeHint() const {
    return sizeHint();
}

void RenderProgressPie::setDisplay(const RenderActivity::Display& display) {
    display_ = display;
    const QString text = describe();
    if (toolTip() != text) {
        setToolTip(text);
        setAccessibleDescription(text);
    }
    const int steps = stepsOf(display);
    // Repaint only when the shown state changes.
    if (steps != shownSteps_ || display.visible != shownRendering_) {
        shownSteps_ = steps;
        shownRendering_ = display.visible;
        update();
    }
}

void RenderProgressPie::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int side = std::min(width(), height()) - 2 * margin;
    const QRectF rect(QPointF(width() - side, height() - side) / 2.0, QSizeF(side, side));
    const QColor colour = shownRendering_ ? theme::progressBusy : theme::progressDone;

    // The outline of the whole pie, in the state colour, so a render with no fraction yet is an
    // empty red pie.
    painter.setPen(QPen(colour, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(rect);

    if (shownSteps_ <= 0) {
        return;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(colour);
    if (shownSteps_ >= resolution) {
        painter.drawEllipse(rect);
        return;
    }
    // Angles are in sixteenths of a degree, counter-clockwise from three o'clock; start at
    // twelve and run clockwise.
    constexpr int quarter = 90 * 16;
    const int span = -static_cast<int>(std::lround(360.0 * 16.0 * shownSteps_ / resolution));
    painter.drawPie(rect, quarter, span);
}

} // namespace arraw::app
