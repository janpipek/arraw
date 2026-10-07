#include "RenderProgressPie.h"

#include "ThemeColors.h"

#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace arraw::app {

namespace {

/// Margin between the widget's edge and the pie, in pixels.
constexpr int margin = 1;
/// Width of the outline, in pixels.
constexpr qreal penWidth = 1.0;

/// @brief Gives the fill in steps of the resolution: the fraction while a render is shown, full
/// when up to date, empty with no photograph, and empty for a render without a fraction.
int stepsOf(const RenderActivity::Display& display, bool photoOpen) {
    if (!display.visible) {
        return photoOpen ? RenderProgressPie::resolution : 0;
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
    // As wide as it is tall at the font's height, and as tall as the status bar gives.
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setAccessibleDescription(describe());
    setToolTip(describe());
}

double RenderProgressPie::filled() const noexcept {
    if (failed_) {
        return 0.0;
    }
    return static_cast<double>(stepsOf(display_, photoOpen_)) / resolution;
}

QString RenderProgressPie::describe() const {
    if (failed_) {
        return tr("Render failed: %1").arg(*failed_);
    }
    if (!display_.visible) {
        return photoOpen_ ? tr("Up to date") : tr("No photograph open");
    }
    QString text = renderStepText(display_.step);
    if (display_.fraction) {
        text += QStringLiteral(" %1%").arg(std::lround(filled() * 100));
    }
    return text;
}

QSize RenderProgressPie::sizeHint() const {
    // The height of a line of the status bar's text; the pie takes the bar's height from its
    // layout, and a margin of a pixel around it, so it scales with the font and the display.
    const int side = fontMetrics().height();
    return {side, side};
}

QSize RenderProgressPie::minimumSizeHint() const {
    return sizeHint();
}

void RenderProgressPie::setDisplay(const RenderActivity::Display& display) {
    display_ = display;
    // A render on its way supersedes the failure of an earlier one.
    if (display.visible) {
        failed_.reset();
    }
    refresh();
}

void RenderProgressPie::setFailed(const QString& error) {
    failed_ = error;
    refresh();
}

void RenderProgressPie::setPhotoOpen(bool open) {
    photoOpen_ = open;
    failed_.reset();
    refresh();
}

void RenderProgressPie::refresh() {
    const QString text = describe();
    if (toolTip() != text) {
        setToolTip(text);
        setAccessibleDescription(text);
    }
    const int steps = failed_ ? 0 : stepsOf(display_, photoOpen_);
    const bool shownFailed = failed_.has_value();
    // Repaint only when the shown state changes.
    if (steps != shownSteps_ || display_.visible != shownRendering_ ||
        photoOpen_ != shownPhotoOpen_ || shownFailed != shownFailed_) {
        shownFailed_ = shownFailed;
        shownSteps_ = steps;
        shownRendering_ = display_.visible;
        shownPhotoOpen_ = photoOpen_;
        update();
    }
}

void RenderProgressPie::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int side = std::min(width(), height()) - 2 * margin;
    // Inset by half the pen width, so the outline's pixels lie inside the widget and the ring
    // is not blurred over a neighbouring row.
    const qreal inset = penWidth / 2.0;
    // Whole pixels (integer division), so the half-pen inset lands the stroke on pixel centres.
    const QRectF rect(QPointF((width() - side) / 2, (height() - side) / 2), QSizeF(side, side));
    const QRectF outline = rect.adjusted(inset, inset, -inset, -inset);
    const QColor colour = (shownRendering_ || shownFailed_) ? theme::progressBusy
                          : shownPhotoOpen_                 ? theme::progressDone
                                                            : theme::progressIdle;

    // The fill first, so that the outline of the whole pie, in the state colour, is on top: an
    // empty pie is an outline alone, and a partly filled one is not half one colour.
    if (shownSteps_ > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        if (shownSteps_ >= resolution) {
            painter.drawEllipse(outline);
        } else {
            // Angles are in sixteenths of a degree, counter-clockwise from three o'clock; start
            // at twelve and run clockwise.
            constexpr int quarter = 90 * 16;
            const int span =
                -static_cast<int>(std::lround(360.0 * 16.0 * shownSteps_ / resolution));
            painter.drawPie(outline, quarter, span);
        }
    }
    painter.setPen(QPen(colour, penWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(outline);
}

} // namespace arraw::app
