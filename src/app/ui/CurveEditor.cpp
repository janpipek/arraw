#include "CurveEditor.h"

#include <QColor>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

namespace arraw::app {

namespace {

/// Radius of a drawn control point, in pixels.
constexpr qreal pointRadius = 4.5;

/// Distance from a point that still grabs it, in pixels.
constexpr qreal grabRadius = 9.0;

/// Room around the plot, so that points on its edges are drawn whole, in pixels.
constexpr qreal plotInset = pointRadius + 2.0;

/// Distance beyond the plot at which a dragged point is removed, in pixels.
constexpr qreal removeDistance = 24.0;

/// Step of an arrow key, in the curve's coordinates; Shift takes ten.
constexpr float keyStep = 0.01F;

/// Ticks of the grid across each axis: quarters.
constexpr int gridDivisions = 4;

/// @brief Gives the colour of a channel's curve.
QColor curveColour(CurveChannel channel, const QPalette& palette) {
    switch (channel) {
    case CurveChannel::Luma:
        return palette.color(QPalette::Text);
    case CurveChannel::Red:
        return {220, 60, 60};
    case CurveChannel::Green:
        return {60, 170, 70};
    case CurveChannel::Blue:
        return {70, 120, 230};
    }
    return palette.color(QPalette::Text);
}

/// @brief Gives the counts of the histogram a channel's curve reads.
const CurveHistogram::Bins& binsOf(const CurveHistogram& histogram, CurveChannel channel) {
    switch (channel) {
    case CurveChannel::Luma:
        break;
    case CurveChannel::Red:
        return histogram.red;
    case CurveChannel::Green:
        return histogram.green;
    case CurveChannel::Blue:
        return histogram.blue;
    }
    return histogram.luma;
}

/// @brief Computes the heights of the histogram's bars, from 0 to 1.
///
/// Square roots of the counts, so that a sparse tone range still shows beside
/// a dense one, scaled to the tallest bin between the ends: a spike of
/// clipped black or white must not flatten the rest, and is cut off at the top.
std::vector<qreal> barHeights(const CurveHistogram::Bins& bins) {
    std::uint64_t tallest = *std::max_element(bins.begin() + 1, bins.end() - 1);
    if (tallest == 0) {
        tallest = *std::ranges::max_element(bins);
    }
    std::vector<qreal> heights(bins.size(), 0.0);
    if (tallest == 0) {
        return heights;
    }
    const qreal scale = std::sqrt(static_cast<qreal>(tallest));
    std::ranges::transform(bins, heights.begin(), [scale](std::uint64_t count) {
        return std::min(1.0, std::sqrt(static_cast<qreal>(count)) / scale);
    });
    return heights;
}

} // namespace

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    // Tab or a click gives the keyboard to the selected point; Esc or Enter
    // gives it back. Unlike SettingSlider's NoFocus: the editor has no spin box,
    // so it is its own keyboard way to a value (ADR 036).
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setToolTip(tr("Click to add a point, drag to move it. Double-click, right-click or drag a "
                  "point out to remove it."));
    setAccessibleName(tr("Tone curve"));
    setAccessibleDescription(
        tr("Curve of the channel shown, input across and output up. Click to add a point and "
           "drag to move it; double-click, right-click or drag a point out to remove it. Page Up "
           "and Page Down select a point, the arrow keys move it, Delete removes it, and Esc or "
           "Enter return to the photograph."));
    // As SettingSlider's: bridges keyboard auto-repeat into one edit.
    pendingTimer_.setSingleShot(true);
    pendingTimer_.setInterval(500);
    connect(&pendingTimer_, &QTimer::timeout, this, &CurveEditor::finishPendingEdit);
}

void CurveEditor::setCurves(const ToneCurveSettings& curves) {
    curves_ = curves;
    if (selected_ && *selected_ >= curve().points.size()) {
        selected_.reset();
    }
    if (drag_ && !dragEditing_ && drag_->start() != curve()) {
        // The curve changed under a press that had not moved yet: start from it.
        drag_.reset();
    }
    refresh();
}

void CurveEditor::setChannel(CurveChannel channel) {
    if (channel == channel_) {
        return;
    }
    finishPendingEdit();
    endDrag();
    channel_ = channel;
    selected_.reset();
    refresh();
}

void CurveEditor::setHistogram(std::optional<CurveHistogram> histogram) {
    histogram_ = std::move(histogram);
    refresh();
}

void CurveEditor::resetChannel() {
    if (curve().isIdentity()) {
        return;
    }
    finishPendingEdit();
    endDrag();
    selected_.reset();
    emit editStarted();
    apply(ToneCurve{});
    emit editFinished();
}

void CurveEditor::finishPendingEdit() {
    if (!pending_) {
        return;
    }
    pending_ = false;
    pendingTimer_.stop();
    emit editFinished();
}

QRectF CurveEditor::plotRect() const {
    const qreal side = std::max(0.0, std::min(width(), height()) - 2.0 * plotInset);
    return {(width() - side) / 2.0, plotInset, side, side};
}

QPointF CurveEditor::toWidget(CurvePoint point) const {
    const QRectF plot = plotRect();
    return {plot.left() + point.x * plot.width(), plot.bottom() - point.y * plot.height()};
}

CurvePoint CurveEditor::toCurve(QPointF position) const {
    const QRectF plot = plotRect();
    if (plot.width() <= 0.0) {
        return {};
    }
    return {static_cast<float>((position.x() - plot.left()) / plot.width()),
            static_cast<float>((plot.bottom() - position.y()) / plot.height())};
}

bool CurveEditor::hasHeightForWidth() const {
    return true;
}

int CurveEditor::heightForWidth(int width) const {
    return width;
}

QSize CurveEditor::sizeHint() const {
    return {240, 240};
}

QSize CurveEditor::minimumSizeHint() const {
    return {120, 120};
}

const ToneCurve& CurveEditor::curve() const {
    return curveOf(curves_, channel_);
}

std::optional<std::size_t> CurveEditor::pointAt(QPointF position) const {
    const QRectF plot = plotRect();
    if (plot.width() <= 0.0) {
        return std::nullopt;
    }
    return pointNear(curve(), toCurve(position), static_cast<float>(grabRadius / plot.width()));
}

bool CurveEditor::apply(const ToneCurve& edited) {
    if (edited == curve()) {
        return false;
    }
    curveOf(curves_, channel_) = edited;
    refresh();
    emit curveEdited(channel_, edited);
    return true;
}

void CurveEditor::removeAsEdit(std::size_t index) {
    ToneCurve edited = curve();
    if (!removePoint(edited, index)) {
        return;
    }
    finishPendingEdit();
    endDrag();
    selected_.reset();
    emit editStarted();
    apply(edited);
    emit editFinished();
}

void CurveEditor::nudge(float dx, float dy) {
    if (!selected_) {
        return;
    }
    const std::size_t index = *selected_;
    const CurvePoint& point = curve().points[index];
    ToneCurve edited = curve();
    edited.points[index] = clampedPosition(curve(), index, {point.x + dx, point.y + dy});
    if (edited == curve()) {
        return;
    }
    if (!pending_) {
        pending_ = true;
        emit editStarted();
    }
    apply(edited);
    pendingTimer_.start();
}

void CurveEditor::refresh() {
    update();
    QString text;
    if (selected_ && *selected_ < curve().points.size()) {
        const CurvePoint& point = curve().points[*selected_];
        text = tr("In %1 \u2192 Out %2").arg(point.x, 0, 'f', 2).arg(point.y, 0, 'f', 2);
    }
    if (text != readout_) {
        readout_ = text;
        emit readoutChanged(readout_);
    }
}

void CurveEditor::endDrag() {
    if (!drag_) {
        return;
    }
    drag_.reset();
    if (dragEditing_) {
        dragEditing_ = false;
        emit editFinished();
    }
}

void CurveEditor::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = plotRect();
    const QPalette& colours = palette();

    painter.fillRect(plot, colours.color(QPalette::Base));

    const QColor colour = curveColour(channel_, colours);
    if (histogram_) {
        const std::vector<qreal> heights = barHeights(binsOf(*histogram_, channel_));
        const qreal binWidth = plot.width() / static_cast<qreal>(heights.size());
        QPainterPath area;
        area.moveTo(plot.bottomLeft());
        for (std::size_t bin = 0; bin < heights.size(); ++bin) {
            const qreal top = plot.bottom() - heights[bin] * plot.height();
            area.lineTo(plot.left() + static_cast<qreal>(bin) * binWidth, top);
            area.lineTo(plot.left() + static_cast<qreal>(bin + 1) * binWidth, top);
        }
        area.lineTo(plot.bottomRight());
        area.closeSubpath();
        QColor fill = channel_ == CurveChannel::Luma ? colours.color(QPalette::Mid) : colour;
        fill.setAlpha(channel_ == CurveChannel::Luma ? 140 : 70);
        painter.fillPath(area, fill);
    }

    const qreal ratio = devicePixelRatioF();
    QColor grid = colours.color(QPalette::Mid);
    painter.setPen(QPen(grid, 1.0));
    for (int line = 1; line < gridDivisions; ++line) {
        const qreal offset = plot.width() * line / gridDivisions;
        // On the middle of a device pixel, so that the line is one crisp pixel.
        const qreal x = std::floor((plot.left() + offset) * ratio) / ratio + 0.5 / ratio;
        const qreal y = std::floor((plot.top() + offset) * ratio) / ratio + 0.5 / ratio;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    grid.setAlpha(120);
    painter.setPen(QPen(grid, 1.0, Qt::DashLine));
    painter.drawLine(plot.bottomLeft(), plot.topRight());
    painter.setPen(QPen(colours.color(QPalette::Mid), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(std::floor(plot.left() * ratio) / ratio + 0.5 / ratio,
                            std::floor(plot.top() * ratio) / ratio + 0.5 / ratio,
                            std::floor(plot.width() * ratio) / ratio,
                            std::floor(plot.height() * ratio) / ratio));

    const ToneCurve& shown = curve();
    const auto count = static_cast<std::size_t>(std::max(64.0, plot.width() * ratio));
    std::vector<float> values;
    try {
        values = sampleCurve(shown, count);
    } catch (const std::exception&) {
        // A malformed curve from outside the editor: only its points are drawn.
    }
    painter.save();
    painter.setClipRect(plot.adjusted(-1.0, -1.0, 1.0, 1.0));
    painter.setPen(QPen(colour, 1.5));
    QPolygonF line;
    line.reserve(static_cast<qsizetype>(values.size()));
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(count - 1);
        line.append(toWidget({x, values[i]}));
    }
    painter.drawPolyline(line);
    painter.restore();

    for (std::size_t index = 0; index < shown.points.size(); ++index) {
        const bool selected = selected_ == index;
        painter.setPen(QPen(colour, 1.5));
        painter.setBrush(selected ? QBrush(colour) : QBrush(colours.color(QPalette::Base)));
        painter.drawEllipse(toWidget(shown.points[index]), pointRadius, pointRadius);
    }
    if (hasFocus() && selected_) {
        painter.setPen(QPen(colours.color(QPalette::Highlight), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(toWidget(shown.points[*selected_]), pointRadius + 3.0,
                            pointRadius + 3.0);
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* event) {
    const QPointF position = event->position();
    const std::optional<std::size_t> hit = pointAt(position);
    if (event->button() == Qt::RightButton) {
        if (hit) {
            removeAsEdit(*hit);
        }
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    finishPendingEdit();
    endDrag();
    addedByPress_.reset();
    if (hit) {
        selected_ = hit;
        drag_.emplace(curve(), *hit);
        refresh();
        event->accept();
        return;
    }
    if (!plotRect().contains(position)) {
        return;
    }
    ToneCurve edited = curve();
    const std::optional<std::size_t> added = insertPoint(edited, toCurve(position));
    if (!added) {
        return;
    }
    // Adding is the start of a drag of the new point: one edit, from press to release.
    emit editStarted();
    dragEditing_ = true;
    selected_ = added;
    addedByPress_ = added;
    drag_.emplace(edited, *added);
    apply(edited);
    event->accept();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_ || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPointF position = event->position();
    const bool outside =
        !plotRect()
             .adjusted(-removeDistance, -removeDistance, removeDistance, removeDistance)
             .contains(position);
    const ToneCurve edited = drag_->curveAt(toCurve(position), outside);
    const bool removed = edited.points.size() < drag_->start().points.size();
    if (edited == curve()) {
        return;
    }
    if (!dragEditing_) {
        dragEditing_ = true;
        emit editStarted();
    }
    if (removed) {
        selected_.reset();
    } else {
        selected_ = drag_->index();
    }
    apply(edited);
}

void CurveEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && drag_) {
        endDrag();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // The press of this double-click may just have added the point under it.
        const std::optional<std::size_t> hit = pointAt(event->position());
        if (hit && hit != addedByPress_) {
            removeAsEdit(*hit);
            event->accept();
            return;
        }
        if (hit) {
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool CurveEditor::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        // Keys the editor uses are its own while it has the focus, ahead of the
        // window's shortcuts (the arrow keys step between photographs).
        const auto* key = static_cast<QKeyEvent*>(event);
        const bool withPoint = selected_.has_value();
        switch (key->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (withPoint && (key->modifiers() & ~Qt::ShiftModifier) == Qt::NoModifier) {
                event->accept();
                return true;
            }
            break;
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            event->accept();
            return true;
        // Esc is not claimed: while the white balance picker is armed, the
        // window's Esc cancels it; otherwise no shortcut takes Esc, and it
        // reaches keyPressEvent, which hands the focus back.
        default:
            break;
        }
    }
    return QWidget::event(event);
}

void CurveEditor::keyPressEvent(QKeyEvent* event) {
    const float step = (event->modifiers() & Qt::ShiftModifier) ? 10.0F * keyStep : keyStep;
    const std::size_t count = curve().points.size();
    switch (event->key()) {
    case Qt::Key_Left:
        nudge(-step, 0.0F);
        break;
    case Qt::Key_Right:
        nudge(step, 0.0F);
        break;
    case Qt::Key_Up:
        nudge(0.0F, step);
        break;
    case Qt::Key_Down:
        nudge(0.0F, -step);
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (selected_) {
            removeAsEdit(*selected_);
        }
        break;
    case Qt::Key_PageUp:
        finishPendingEdit();
        selected_ = selected_ && *selected_ > 0 ? *selected_ - 1 : count - 1;
        refresh();
        break;
    case Qt::Key_PageDown:
        finishPendingEdit();
        selected_ = selected_ && *selected_ + 1 < count ? *selected_ + 1 : 0;
        refresh();
        break;
    case Qt::Key_Escape:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        finishPendingEdit();
        emit focusReleased();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void CurveEditor::focusOutEvent(QFocusEvent* event) {
    finishPendingEdit();
    refresh();
    QWidget::focusOutEvent(event);
}

} // namespace arraw::app
