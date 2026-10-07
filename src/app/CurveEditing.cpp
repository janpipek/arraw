#include "CurveEditing.h"

#include "ToneCurve.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace arraw::app {

const ToneCurve& curveOf(const ToneCurveSettings& curves, CurveChannel channel) {
    switch (channel) {
    case CurveChannel::Luma:
        return curves.luma;
    case CurveChannel::Red:
        return curves.red;
    case CurveChannel::Green:
        return curves.green;
    case CurveChannel::Blue:
        return curves.blue;
    }
    throw std::invalid_argument("not a curve channel");
}

ToneCurve& curveOf(ToneCurveSettings& curves, CurveChannel channel) {
    return const_cast<ToneCurve&>(curveOf(std::as_const(curves), channel));
}

std::optional<std::size_t> pointNear(const ToneCurve& curve, CurvePoint at, float radius) {
    std::optional<std::size_t> nearest;
    float best = radius;
    for (std::size_t index = 0; index < curve.points.size(); ++index) {
        const CurvePoint& point = curve.points[index];
        const float distance = std::hypot(point.x - at.x, point.y - at.y);
        if (distance <= best) {
            best = distance;
            nearest = index;
        }
    }
    return nearest;
}

bool isRemovable(const ToneCurve& curve, std::size_t index) {
    return curve.points.size() > minimumCurvePoints && index > 0 && index + 1 < curve.points.size();
}

CurvePoint clampedPosition(const ToneCurve& curve, std::size_t index, CurvePoint target) {
    const std::vector<CurvePoint>& points = curve.points;
    const float y = std::clamp(target.y, 0.0F, 1.0F);
    if (index == 0) {
        return {0.0F, y};
    }
    if (index + 1 >= points.size()) {
        return {1.0F, y};
    }
    const float low = points[index - 1].x + minimumCurvePointSpacing;
    const float high = points[index + 1].x - minimumCurvePointSpacing;
    // A gap of exactly twice the spacing can round to low > high: the point stays.
    const float x = low <= high ? std::clamp(target.x, low, high) : points[index].x;
    return {x, y};
}

std::optional<std::size_t> insertPoint(ToneCurve& curve, CurvePoint at) {
    std::vector<CurvePoint>& points = curve.points;
    if (points.size() >= maximumCurvePoints || points.size() < minimumCurvePoints ||
        !(at.x > 0.0F && at.x < 1.0F)) {
        return std::nullopt;
    }
    const auto next = std::ranges::find_if(points, [&](const CurvePoint& p) { return p.x > at.x; });
    if (next == points.begin() || next == points.end()) {
        return std::nullopt;
    }
    const float low = std::prev(next)->x + minimumCurvePointSpacing;
    const float high = next->x - minimumCurvePointSpacing;
    if (low > high) {
        return std::nullopt;
    }
    const CurvePoint placed{std::clamp(at.x, low, high), std::clamp(at.y, 0.0F, 1.0F)};
    const auto inserted = points.insert(next, placed);
    return static_cast<std::size_t>(std::distance(points.begin(), inserted));
}

bool removePoint(ToneCurve& curve, std::size_t index) {
    if (!isRemovable(curve, index)) {
        return false;
    }
    curve.points.erase(curve.points.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

CurvePointDrag::CurvePointDrag(ToneCurve curve, std::size_t index)
    : start_(std::move(curve)), index_(index) {
    if (index_ >= start_.points.size()) {
        throw std::out_of_range("no such curve point");
    }
}

ToneCurve CurvePointDrag::curveAt(CurvePoint target, bool outside) const {
    ToneCurve curve = start_;
    if (outside && removePoint(curve, index_)) {
        return curve;
    }
    curve.points[index_] = clampedPosition(start_, index_, target);
    return curve;
}

std::vector<float> sampleCurve(const ToneCurve& curve, std::size_t count) {
    if (count < 2) {
        throw std::invalid_argument("a curve is sampled at two inputs at least");
    }
    const CurvePlan plan = curvePlanFor(curve);
    std::vector<float> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(count - 1);
        // The identity resolves to an inactive plan, which the chain skips.
        values[i] = plan.active ? evaluateCurve(plan, x) : x;
    }
    return values;
}

} // namespace arraw::app
