#include "TestApp.h"
#include "ui/ToneCurveWidget.h"
#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QMouseEvent>

using Channel = ToneCurveWidget::Channel;

namespace {

// Deliver a synthetic mouse event straight to the widget's handlers, the same
// way QWidgetWindow routes real input. Position is in widget-local coords.
void sendMouse(QWidget& w, QEvent::Type type, QPointF pos, Qt::MouseButton button) {
    const Qt::MouseButtons buttons = (type == QEvent::MouseMove) ? Qt::LeftButton : button;
    QMouseEvent ev(type, pos, w.mapToGlobal(pos.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&w, &ev);
}

// Widget-space pixel for a curve-space point, mirroring ToneCurveWidget::toWidget.
QPointF widgetPos(const QWidget& w, QPointF curve, int pad = 10) {
    const int pw = w.width() - (2 * pad);
    const int ph = w.height() - (2 * pad);
    return {pad + (curve.x() * pw), pad + ((1.0 - curve.y()) * ph)};
}

} // namespace

// Regression: clicking the identity curve inserts a point and immediately
// begins dragging it. The first insert reallocates the 2-element vector, so the
// drag index must be computed without relying on the unsequenced
// `insert() - begin()`; otherwise the drag starts on a garbage index and the
// next mouse-move aborts out of bounds.
TEST_CASE("insert-then-drag a fresh point does not crash", "[tonecurve]") {
    testApp();
    ToneCurveWidget w;
    w.resize(220, 180);

    // Click mid-diagonal on the default identity curve to insert a point.
    const QPointF grab = widgetPos(w, {0.5, 0.5});
    sendMouse(w, QEvent::MouseButtonPress, grab, Qt::LeftButton);
    REQUIRE(w.points(Channel::Luma).size() == 3);

    // Drag the new (interior) point upward; this is where a bad index aborts.
    for (int d = 1; d <= 20; ++d)
        sendMouse(w, QEvent::MouseMove, grab + QPointF(d, -d), Qt::NoButton);
    sendMouse(w, QEvent::MouseButtonRelease, grab + QPointF(20, -20), Qt::LeftButton);

    const auto pts = w.points(Channel::Luma);
    REQUIRE(pts.size() == 3);
    CHECK(pts[1].y() > 0.5); // the dragged point moved up
}

// Regression: an undo (setPoints) firing mid-drag replaces the points the drag
// is indexing. The stale dragIndex must not survive into the next mouse-move,
// which would otherwise index the shorter vector out of bounds and abort.
TEST_CASE("setPoints mid-drag cancels the drag instead of crashing", "[tonecurve]") {
    testApp();
    ToneCurveWidget w;
    w.resize(220, 180);

    // Four points so the interior index we grab (2) is past the end of the
    // identity curve we replace it with.
    w.setPoints(Channel::Luma, {{0.0, 0.0}, {0.3, 0.35}, {0.6, 0.6}, {1.0, 1.0}});

    const QPointF grab = widgetPos(w, {0.6, 0.6});
    sendMouse(w, QEvent::MouseButtonPress, grab, Qt::LeftButton);

    // The undo: collapse back to a two-point identity curve while still dragging.
    const std::vector<QPointF> identity = {{0.0, 0.0}, {1.0, 1.0}};
    w.setPoints(Channel::Luma, identity);

    // Would dereference points[2] on a 2-element vector before the fix.
    sendMouse(w, QEvent::MouseMove, grab + QPointF(5, -5), Qt::NoButton);
    sendMouse(w, QEvent::MouseButtonRelease, grab + QPointF(5, -5), Qt::LeftButton);

    // Drag was abandoned: the identity curve is untouched.
    REQUIRE(w.points(Channel::Luma) == identity);
}

// Switching channel mid-drag (e.g. a keyboard shortcut) makes a different,
// shorter points vector current; the drag index from the old channel is stale.
TEST_CASE("setChannel mid-drag cancels the drag instead of crashing", "[tonecurve]") {
    testApp();
    ToneCurveWidget w;
    w.resize(220, 180);

    w.setPoints(Channel::Luma, {{0.0, 0.0}, {0.3, 0.35}, {0.6, 0.6}, {1.0, 1.0}});

    const QPointF grab = widgetPos(w, {0.6, 0.6});
    sendMouse(w, QEvent::MouseButtonPress, grab, Qt::LeftButton);

    w.setChannel(Channel::Red); // Red still holds the 2-point identity curve

    sendMouse(w, QEvent::MouseMove, grab + QPointF(5, -5), Qt::NoButton);
    sendMouse(w, QEvent::MouseButtonRelease, grab + QPointF(5, -5), Qt::LeftButton);

    const std::vector<QPointF> identity = {{0.0, 0.0}, {1.0, 1.0}};
    REQUIRE(w.points(Channel::Red) == identity);
}
