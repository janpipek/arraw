#include "ToneCurve.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;

namespace {

/// @brief Tangents of the monotone cubic through the points, in the Fritsch--Carlson construction.
std::vector<double> tangentsFor(const std::vector<double>& secants) {
    const std::size_t count = secants.size() + 1;
    std::vector<double> tangents(count);
    tangents.front() = secants.front();
    tangents.back() = secants.back();
    for (std::size_t i = 1; i + 1 < count; ++i) {
        // Opposite slopes on either side, or a flat side, make a turning point.
        tangents[i] =
            secants[i - 1] * secants[i] <= 0.0 ? 0.0 : (secants[i - 1] + secants[i]) / 2.0;
    }
    for (std::size_t i = 0; i + 1 < count; ++i) {
        if (secants[i] == 0.0) {
            tangents[i] = 0.0;
            tangents[i + 1] = 0.0;
            continue;
        }
        const double alpha = tangents[i] / secants[i];
        const double beta = tangents[i + 1] / secants[i];
        const double size = alpha * alpha + beta * beta;
        if (size > 9.0) {
            const double scale = 3.0 / std::sqrt(size);
            tangents[i] = scale * alpha * secants[i];
            tangents[i + 1] = scale * beta * secants[i];
        }
    }
    return tangents;
}

} // namespace

CurvePlan arraw::curvePlanFor(const ToneCurve& curve) {
    if (!isWellFormed(curve)) {
        throw std::invalid_argument(std::string("a tone curve needs ") + toneCurveRequirements);
    }
    CurvePlan plan;
    if (curve.isIdentity()) {
        return plan;
    }
    plan.active = true;

    const std::vector<CurvePoint>& points = curve.points;
    std::vector<double> secants(points.size() - 1);
    for (std::size_t i = 0; i < secants.size(); ++i) {
        secants[i] = (static_cast<double>(points[i + 1].y) - points[i].y) /
                     (static_cast<double>(points[i + 1].x) - points[i].x);
    }
    const std::vector<double> tangents = tangentsFor(secants);

    std::size_t segment = 0;
    for (std::size_t i = 0; i < toneCurveSamples; ++i) {
        const double x = static_cast<double>(i) / static_cast<double>(toneCurveSamples - 1);
        while (segment + 2 < points.size() && x > points[segment + 1].x) {
            ++segment;
        }
        const double x0 = points[segment].x;
        const double width = static_cast<double>(points[segment + 1].x) - x0;
        const double t = std::clamp((x - x0) / width, 0.0, 1.0);
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double y = (2.0 * t3 - 3.0 * t2 + 1.0) * points[segment].y +
                         (t3 - 2.0 * t2 + t) * width * tangents[segment] +
                         (-2.0 * t3 + 3.0 * t2) * points[segment + 1].y +
                         (t3 - t2) * width * tangents[segment + 1];
        plan.table[i] = static_cast<float>(y);
    }
    // The ends are the control points themselves, not their rounded neighbours.
    plan.table.front() = points.front().y;
    plan.table.back() = points.back().y;
    return plan;
}

ToneCurvePlan arraw::toneCurvePlanFor(const ToneCurveSettings& settings) {
    return {curvePlanFor(settings.luma), curvePlanFor(settings.red), curvePlanFor(settings.green),
            curvePlanFor(settings.blue)};
}
