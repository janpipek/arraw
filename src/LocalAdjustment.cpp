#include "LocalAdjustment.h"

#include <algorithm>
#include <limits>

namespace {
// Squared distance between two normalised points in aspect-corrected space.
double aspectDist2(QPointF a, QPointF b, double aspect) {
    const double dx = (a.x() - b.x()) * aspect;
    const double dy = a.y() - b.y();
    return dx * dx + dy * dy;
}
}  // namespace

float maskWeight(const LinearMask& m, QPointF uv, float aspect) {
    // Evaluate in aspect-corrected space (x scaled by aspect) so the gradient
    // stays perpendicular to its drawn line on screen. t is a ratio, so the
    // overall scale cancels — only the x:y ratio (= aspect) matters.
    const double a = aspect;
    const double dx = (m.p1.x() - m.p0.x()) * a;
    const double dy = m.p1.y() - m.p0.y();
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 0.0)
        return 0.0f;
    const double px = (uv.x() - m.p0.x()) * a;
    const double py = uv.y() - m.p0.y();
    const double t = std::clamp((px * dx + py * dy) / len2, 0.0, 1.0);
    return static_cast<float>(t * t * (3.0 - 2.0 * t));  // smoothstep falloff
}

LinearHandle nearestHandle(const LinearMask& m, QPointF cursor, float aspect,
                           double pickRadius) {
    const QPointF center = (m.p0 + m.p1) / 2.0;
    const struct {
        LinearHandle handle;
        double dist2;
    } candidates[] = {
        {LinearHandle::P0, aspectDist2(cursor, m.p0, aspect)},
        {LinearHandle::P1, aspectDist2(cursor, m.p1, aspect)},
        {LinearHandle::Center, aspectDist2(cursor, center, aspect)},
    };

    LinearHandle best = LinearHandle::None;
    double bestDist2 = std::numeric_limits<double>::infinity();
    for (const auto& c : candidates) {
        if (c.dist2 < bestDist2) {  // strict: endpoints win ties over center
            best = c.handle;
            bestDist2 = c.dist2;
        }
    }
    return bestDist2 <= pickRadius * pickRadius ? best : LinearHandle::None;
}

LinearMask moveHandle(LinearMask m, LinearHandle h, QPointF to) {
    switch (h) {
    case LinearHandle::P0:
        m.p0 = to;
        break;
    case LinearHandle::P1:
        m.p1 = to;
        break;
    case LinearHandle::Center: {
        const QPointF delta = to - (m.p0 + m.p1) / 2.0;
        m.p0 += delta;
        m.p1 += delta;
        break;
    }
    case LinearHandle::None:
        break;
    }
    return m;
}
