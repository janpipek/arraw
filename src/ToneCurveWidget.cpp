#include "ToneCurveWidget.h"
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QPainter>
#include <QPainterPath>
#include <QAction>
#include <QMenu>
#include <algorithm>

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

std::vector<QPointF>& ToneCurveWidget::currentPoints() {
    switch (currentChannel) {
    case Channel::Red:   return pointsR;
    case Channel::Green: return pointsG;
    case Channel::Blue:  return pointsB;
    default:             return pointsLuma;
    }
}

const std::vector<QPointF>& ToneCurveWidget::currentPoints() const {
    switch (currentChannel) {
    case Channel::Red:   return pointsR;
    case Channel::Green: return pointsG;
    case Channel::Blue:  return pointsB;
    default:             return pointsLuma;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void ToneCurveWidget::setChannel(Channel ch) {
    if (currentChannel == ch) return;
    currentChannel = ch;
    update();
}

void ToneCurveWidget::setPoints(Channel ch, const std::vector<QPointF>& pts) {
    switch (ch) {
    case Channel::Luma:  pointsLuma = pts; break;
    case Channel::Red:   pointsR    = pts; break;
    case Channel::Green: pointsG    = pts; break;
    case Channel::Blue:  pointsB    = pts; break;
    }
    update();
}

const std::vector<QPointF>& ToneCurveWidget::points(Channel ch) const {
    switch (ch) {
    case Channel::Red:   return pointsR;
    case Channel::Green: return pointsG;
    case Channel::Blue:  return pointsB;
    default:             return pointsLuma;
    }
}

void ToneCurveWidget::resetChannel(Channel ch) {
    const std::vector<QPointF> identity = {{0.0, 0.0}, {1.0, 1.0}};
    setPoints(ch, identity);
    emit curveChanged(ch, identity);
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void ToneCurveWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF area(kPad, kPad, width() - 2*kPad, height() - 2*kPad);

    // Background
    p.fillRect(rect(), QColor(28, 28, 28));
    p.fillRect(area.toRect(), QColor(22, 22, 22));

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

    // Curve colour by channel
    QColor curveColor;
    switch (currentChannel) {
    case Channel::Red:   curveColor = QColor(220, 80, 80);  break;
    case Channel::Green: curveColor = QColor(80, 180, 80);  break;
    case Channel::Blue:  curveColor = QColor(80, 130, 220); break;
    default:             curveColor = Qt::white;             break;
    }

    // Draw the spline as a polyline of many samples
    const auto& pts = currentPoints();
    if (pts != lutSource) {
        lutCache  = computeCurveLUT(pts);
        lutSource = pts;
    }
    const auto& lut = lutCache;

    QPainterPath curvePath;
    for (int i = 0; i < 256; ++i) {
        const QPointF wp = toWidget({i / 255.0, double(lut[i])});
        if (i == 0) curvePath.moveTo(wp);
        else        curvePath.lineTo(wp);
    }
    p.setPen(QPen(curveColor, 1.5));
    p.drawPath(curvePath);

    // Control point handles
    p.setPen(QPen(Qt::white, 1));
    for (const auto& pt : pts) {
        QPointF wp = toWidget(pt);
        p.setBrush(curveColor);
        p.drawEllipse(wp, kRadius, kRadius);
    }
}

// ── Mouse events ──────────────────────────────────────────────────────────────

void ToneCurveWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    int hit = hitTest(e->position());
    if (hit >= 0) {
        dragIndex = hit;
        dragging  = true;
        emit editingStarted();
    }
}

void ToneCurveWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging || dragIndex < 0) return;

    auto& pts = currentPoints();
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
