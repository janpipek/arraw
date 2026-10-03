#include "ToneCurveSettings.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdlib>
#include <utility>

using namespace arraw;

bool arraw::isWellFormed(const ToneCurve& curve) {
    const std::vector<CurvePoint>& points = curve.points;
    if (points.size() < minimumCurvePoints || points.size() > maximumCurvePoints) {
        return false;
    }
    const bool inRange = std::ranges::all_of(points, [](const CurvePoint& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && point.x >= 0.0F &&
               point.x <= 1.0F && point.y >= 0.0F && point.y <= 1.0F;
    });
    if (!inRange || points.front().x != 0.0F || points.back().x != 1.0F) {
        return false;
    }
    return std::ranges::adjacent_find(points, [](const CurvePoint& a, const CurvePoint& b) {
               return !(b.x - a.x + curveCoordinateTolerance >= minimumCurvePointSpacing);
           }) == points.end();
}

void arraw::normaliseCurvePoints(std::vector<CurvePoint>& points) {
    // A total order, so that a NaN x (which isWellFormed refuses) cannot break the sort.
    std::ranges::stable_sort(points, [](const CurvePoint& a, const CurvePoint& b) {
        return std::strong_order(a.x, b.x) < 0;
    });
    if (points.empty()) {
        return;
    }
    if (std::abs(points.front().x) <= curveCoordinateTolerance) {
        points.front().x = 0.0F;
    }
    if (std::abs(points.back().x - 1.0F) <= curveCoordinateTolerance) {
        points.back().x = 1.0F;
    }
}

std::optional<ToneCurve> arraw::curveFromPoints(std::vector<CurvePoint> points) {
    // The codec and the command line refuse a non-finite x before it reaches here.
    if (points.empty() || !std::ranges::all_of(points, [](const CurvePoint& point) {
            return std::isfinite(point.x);
        })) {
        return std::nullopt;
    }
    normaliseCurvePoints(points);
    ToneCurve curve{std::move(points)};
    if (!isWellFormed(curve)) {
        return std::nullopt;
    }
    return curve;
}
