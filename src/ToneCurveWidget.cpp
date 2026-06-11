#include "ToneCurveWidget.h"
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QPainter>
#include <QPainterPath>
#include <QAction>
#include <QMenu>
#include <algorithm>
#include <cmath>
#include <limits>

ToneCurveWidget::ToneCurveWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

QPointF ToneCurveWidget::toWidget(QPointF curve) const {
    const int pw = width()  - 2 * kPad;
    const int ph = height() - 2 * kPad;
    return {kPad + curve.x() * pw, kPad + (1.0 - curve.y()) * ph};
}

QPointF ToneCurveWidget::toCurve(QPointF widget) const {
    const int pw = width()  - 2 * kPad;
    const int ph = height() - 2 * kPad;
    return {(widget.x() - kPad) / pw, 1.0 - (widget.y() - kPad) / ph};
}

int ToneCurveWidget::hitTest(QPointF pos) const {
    const auto& pts = currentPoints();
    for (int i = 0; i < int(pts.size()); ++i) {
        QPointF d = pos - toWidget(pts[i]);
        if (d.x()*d.x() + d.y()*d.y() < (kRadius*2) * (kRadius*2))
            return i;
    }
    return -1;
}

std::vector<QPointF>& ToneCurveWidget::pointsFor(Channel ch) {
    switch (ch) {
    case Channel::Red:   return pointsR;
    case Channel::Green: return pointsG;
    case Channel::Blue:  return pointsB;
    default:             return pointsLuma;
    }
}

std::vector<QPointF>& ToneCurveWidget::currentPoints() {
    return pointsFor(currentChannel);
}

const std::vector<QPointF>& ToneCurveWidget::currentPoints() const {
    return const_cast<ToneCurveWidget*>(this)->pointsFor(currentChannel);
}

const std::array<float, 256>& ToneCurveWidget::lutFor(Channel ch) {
    const int i = int(ch);
    const auto& pts = pointsFor(ch);
    if (pts != lutSource[i]) {
        lutCache[i]  = computeCurveLUT(pts);
        lutSource[i] = pts;
    }
    return lutCache[i];
}

QColor ToneCurveWidget::channelColor(Channel ch) {
    switch (ch) {
    case Channel::Red:   return {220, 80, 80};
    case Channel::Green: return {80, 180, 80};
    case Channel::Blue:  return {80, 130, 220};
    default:             return Qt::white;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void ToneCurveWidget::setChannel(Channel ch) {
    if (currentChannel == ch) return;
    currentChannel = ch;
    update();
}

void ToneCurveWidget::setPoints(Channel ch, const std::vector<QPointF>& pts) {
    pointsFor(ch) = pts;
    update();
}

const std::vector<QPointF>& ToneCurveWidget::points(Channel ch) const {
    return const_cast<ToneCurveWidget*>(this)->pointsFor(ch);
}

void ToneCurveWidget::resetChannel(Channel ch) {
    const std::vector<QPointF> identity = {{0.0, 0.0}, {1.0, 1.0}};
    setPoints(ch, identity);
    emit curveChanged(ch, identity);
}

void ToneCurveWidget::setHistogramSample(const QImage& img) {
    std::array<std::array<int, kHistBins>, 4> counts{};

    const QImage sample = img.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < sample.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
        for (int x = 0; x < sample.width(); ++x) {
            const int r = qRed(line[x]);
            const int g = qGreen(line[x]);
            const int b = qBlue(line[x]);
            const int luma = int(kLumaR * r + kLumaG * g + kLumaB * b + 0.5f);
            ++counts[int(Channel::Luma)] [luma * kHistBins / 256];
            ++counts[int(Channel::Red)]  [r    * kHistBins / 256];
            ++counts[int(Channel::Green)][g    * kHistBins / 256];
            ++counts[int(Channel::Blue)] [b    * kHistBins / 256];
        }
    }

    // Log-normalise each channel to 0..1 (same scaling as the panel histogram).
    for (int ch = 0; ch < 4; ++ch) {
        int peak = 1;
        for (int i = 0; i < kHistBins; ++i)
            peak = std::max(peak, counts[ch][i]);
        const float scale = 1.0f / std::log1p(float(peak));
        for (int i = 0; i < kHistBins; ++i)
            hist[ch][i] = std::log1p(float(counts[ch][i])) * scale;
    }

    hasHist = true;
    update();
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void ToneCurveWidget::drawHistogram(QPainter& p, const QRectF& area) const {
    const auto& bins = hist[int(currentChannel)];

    QPainterPath silhouette;
    silhouette.moveTo(area.bottomLeft());
    for (int i = 0; i < kHistBins; ++i) {
        const qreal x = area.left() + area.width() * (i + 0.5) / kHistBins;
        const qreal y = area.bottom() - area.height() * bins[i];
        silhouette.lineTo(x, y);
    }
    silhouette.lineTo(area.bottomRight());
    silhouette.closeSubpath();

    QColor fill = currentChannel == Channel::Luma
        ? QColor(130, 130, 130) : channelColor(currentChannel);
    fill.setAlpha(60);
    p.fillPath(silhouette, fill);
}

void ToneCurveWidget::drawCurve(QPainter& p, Channel ch, const QPen& pen) {
    const auto& lut = lutFor(ch);
    QPainterPath path;
    for (int i = 0; i < 256; ++i) {
        const QPointF wp = toWidget({i / 255.0, double(lut[i])});
        if (i == 0) path.moveTo(wp);
        else        path.lineTo(wp);
    }
    p.setPen(pen);
    p.drawPath(path);
}

void ToneCurveWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF area(kPad, kPad, width() - 2*kPad, height() - 2*kPad);

    // Background
    p.fillRect(rect(), QColor(28, 28, 28));
    p.fillRect(area.toRect(), QColor(22, 22, 22));

    if (hasHist)
        drawHistogram(p, area);

    // Grid (4×4)
    p.setPen(QPen(QColor(50, 50, 50), 0.5));
    for (int i = 1; i < 4; ++i) {
        const qreal x = area.left() + area.width()  * i / 4.0;
        const qreal y = area.top()  + area.height() * i / 4.0;
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }

    // Diagonal reference
    p.setPen(QPen(QColor(60, 60, 60), 1, Qt::DashLine));
    p.drawLine(area.bottomLeft(), area.topRight());

    // Ghosts of the other channels' non-identity curves
    for (Channel ch : {Channel::Luma, Channel::Red, Channel::Green, Channel::Blue}) {
        if (ch == currentChannel || isIdentityCurve(pointsFor(ch)))
            continue;
        QColor ghost = channelColor(ch);
        ghost.setAlpha(80);
        drawCurve(p, ch, QPen(ghost, 1.0));
    }

    const QColor curveColor = channelColor(currentChannel);
    drawCurve(p, currentChannel, QPen(curveColor, 1.5));

    // Control point handles
    p.setPen(QPen(Qt::white, 1));
    for (const auto& pt : currentPoints()) {
        QPointF wp = toWidget(pt);
        p.setBrush(curveColor);
        p.drawEllipse(wp, kRadius, kRadius);
    }
}

// ── Mouse events ──────────────────────────────────────────────────────────────

// Insert a point where the click lands on the curve (keeps the curve shape
// unchanged). Returns the new point's index, or -1 if the click was too far.
int ToneCurveWidget::insertPointOnCurve(QPointF pos) {
    const auto& lut = lutFor(currentChannel);

    float best = std::numeric_limits<float>::max();
    for (int i = 0; i < 256; ++i) {
        const QPointF d = pos - toWidget({i / 255.0, double(lut[i])});
        best = std::min(best, float(d.x()*d.x() + d.y()*d.y()));
    }
    if (best > (kRadius*2) * (kRadius*2))
        return -1;

    QPointF cp = toCurve(pos);
    const double x = std::clamp(cp.x(), 0.01, 0.99);
    cp = {x, double(lut[int(x * 255.0 + 0.5)])};

    auto& pts = currentPoints();
    auto  it  = std::lower_bound(pts.begin(), pts.end(), cp,
                    [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    return int(pts.insert(it, cp) - pts.begin());
}

void ToneCurveWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    int hit = hitTest(e->position());
    bool inserted = false;
    if (hit < 0) {
        hit = insertPointOnCurve(e->position());
        inserted = hit >= 0;
    }
    if (hit >= 0) {
        dragIndex = hit;
        dragging  = true;
        emit editingStarted();
        if (inserted)
            emit curveChanged(currentChannel, currentPoints());
        update();
    }
}

void ToneCurveWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging || dragIndex < 0) return;

    auto& pts = currentPoints();

    // Dragging an interior point well outside the widget deletes it.
    const bool interior = dragIndex > 0 && dragIndex < int(pts.size()) - 1;
    if (interior && !rect().adjusted(-kRemoveMargin, -kRemoveMargin,
                                     kRemoveMargin, kRemoveMargin)
                         .contains(e->position().toPoint())) {
        pts.erase(pts.begin() + dragIndex);
        dragIndex = -1;
        update();
        emit curveChanged(currentChannel, pts);
        return;
    }

    QPointF cp = toCurve(e->position());
    cp.setX(std::clamp(cp.x(), 0.0, 1.0));
    cp.setY(std::clamp(cp.y(), 0.0, 1.0));

    // Endpoints stay pinned on x=0 and x=1 (first and last point only)
    if (dragIndex == 0)                   cp.setX(0.0);
    if (dragIndex == int(pts.size()) - 1) cp.setX(1.0);

    // Keep x between neighbours
    if (dragIndex > 0)
        cp.setX(std::max(cp.x(), pts[dragIndex-1].x() + 0.01));
    if (dragIndex < int(pts.size()) - 1)
        cp.setX(std::min(cp.x(), pts[dragIndex+1].x() - 0.01));

    pts[dragIndex] = cp;
    update();
    emit curveChanged(currentChannel, pts);
}

void ToneCurveWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (dragging) {
        dragging  = false;
        dragIndex = -1;
        emit editingFinished();
    }
}

void ToneCurveWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (hitTest(e->position()) >= 0) return; // don't add on existing handle

    QPointF cp = toCurve(e->position());
    cp.setX(std::clamp(cp.x(), 0.01, 0.99));
    cp.setY(std::clamp(cp.y(), 0.0,  1.0));

    auto& pts = currentPoints();
    auto  it  = std::lower_bound(pts.begin(), pts.end(), cp,
                    [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    pts.insert(it, cp);
    update();
    emit editingStarted();
    emit curveChanged(currentChannel, pts);
    emit editingFinished();
}

void ToneCurveWidget::contextMenuEvent(QContextMenuEvent* e) {
    int hit = hitTest(e->pos());
    if (hit < 0) return;

    auto& pts = currentPoints();
    // Don't allow removing the two boundary endpoints
    if (hit == 0 || hit == int(pts.size()) - 1) return;

    QMenu menu(this);
    QAction* removeAct = menu.addAction("Remove point");
    if (menu.exec(e->globalPos()) == removeAct) {
        emit editingStarted();
        pts.erase(pts.begin() + hit);
        update();
        emit curveChanged(currentChannel, pts);
        emit editingFinished();
    }
}
