#include <LensCorrection.h>
#include <NegativeSettings.h>

#include <algorithm>
#include <cmath>

using namespace arraw;

namespace {

/// @brief The frame in the units the tables use: height 1, width the aspect.
struct Frame {
    double width;   ///< Width, in heights.
    double centerX; ///< Optical centre across, in heights.
    double centerY; ///< Optical centre down, in heights.
    double reach;   ///< Distance from the centre to the farthest corner.
};

Frame frameOf(const LensCorrection& correction, double aspect) {
    Frame frame{aspect, correction.centerU * aspect, correction.centerV, 0.0};
    const double far_x = std::max(frame.centerX, aspect - frame.centerX);
    const double far_y = std::max(frame.centerY, 1.0 - frame.centerY);
    frame.reach = std::hypot(far_x, far_y);
    return frame;
}

/// @brief Gives the radial scale of the distortion at a normalised radius; 1 without a table.
double distortionAt(const LensCorrection& correction, double radius) noexcept {
    return correction.distortion.empty() ? 1.0 : tableValue(correction.distortion, radius);
}

/// @brief Maps a point of the corrected frame to the sensor frame, in the tables' units.
void sensorUnits(const LensCorrection& correction, const Frame& frame, double fill, double x,
                 double y, double& sensorX, double& sensorY) noexcept {
    const double ideal_x = frame.centerX + (x - frame.centerX) / fill;
    const double ideal_y = frame.centerY + (y - frame.centerY) / fill;
    const double radius =
        std::hypot(ideal_x - frame.centerX, ideal_y - frame.centerY) / frame.reach;
    const double scale = distortionAt(correction, radius);
    sensorX = frame.centerX + (ideal_x - frame.centerX) * scale;
    sensorY = frame.centerY + (ideal_y - frame.centerY) * scale;
}

} // namespace

double arraw::tableValue(const std::vector<float>& table, double radius) noexcept {
    const auto last = static_cast<double>(table.size() - 1);
    const double position = std::clamp(radius, 0.0, 1.0) * last;
    const auto index = std::min(static_cast<std::size_t>(position), table.size() - 2);
    const double fraction = position - static_cast<double>(index);
    return table[index] + (table[index + 1] - table[index]) * fraction;
}

double arraw::fillScaleOf(const LensCorrection& correction, double aspect) {
    if (correction.distortion.empty() || !(aspect > 0.0)) {
        return 1.0;
    }
    const Frame frame = frameOf(correction, aspect);
    constexpr double largest = 8.0;
    // The boundary of the corrected frame, walked densely, corners included.
    constexpr int steps = 256;
    double fill = 1.0;
    const auto needs = [&](double x, double y) {
        const auto inside = [&](double zoom) {
            double sx = 0.0;
            double sy = 0.0;
            sensorUnits(correction, frame, zoom, x, y, sx, sy);
            // A hair of tolerance keeps rounding in the table from asking for a larger zoom.
            constexpr double slack = 1.0e-9;
            return sx >= -slack && sx <= frame.width + slack && sy >= -slack && sy <= 1.0 + slack;
        };
        if (inside(1.0)) {
            return 1.0;
        }
        double low = 1.0;
        double high = largest;
        if (!inside(high)) {
            return largest;
        }
        for (int round = 0; round < 50; ++round) {
            const double middle = 0.5 * (low + high);
            (inside(middle) ? high : low) = middle;
        }
        return high;
    };
    for (int step = 0; step <= steps; ++step) {
        const double along = static_cast<double>(step) / steps;
        fill = std::max({fill, needs(along * frame.width, 0.0), needs(along * frame.width, 1.0),
                         needs(0.0, along), needs(frame.width, along)});
    }
    return fill;
}

LensCorrection arraw::withFillScale(LensCorrection correction) {
    correction.fill = fillScaleOf(correction, correction.aspect);
    return correction;
}

LensCorrection arraw::withVignettingAmount(LensCorrection correction, float amount) {
    if (correction.vignetting.empty() || amount == profileVignettingAmount) {
        return correction;
    }
    if (amount <= 0.0F) {
        correction.vignetting.clear();
        correction.vignettingFrom.reset();
        return correction.active() ? correction : LensCorrection{};
    }
    const double exponent = static_cast<double>(amount) / 100.0;
    for (float& gain : correction.vignetting) {
        if (std::isfinite(gain) && gain > 0.0F) {
            gain = static_cast<float>(std::pow(static_cast<double>(gain), exponent));
        }
    }
    return correction;
}

double arraw::radiusOf(const LensCorrection& correction, SensorPoint point) noexcept {
    const Frame frame = frameOf(correction, correction.aspect);
    return std::hypot(point.u * frame.width - frame.centerX, point.v - frame.centerY) / frame.reach;
}

SensorPoint arraw::sensorFrom(const LensCorrection& correction,
                              CorrectedPosition position) noexcept {
    if (correction.distortion.empty()) {
        return {static_cast<float>(position.u), static_cast<float>(position.v)};
    }
    const Frame frame = frameOf(correction, correction.aspect);
    double x = 0.0;
    double y = 0.0;
    sensorUnits(correction, frame, correction.fill, position.u * frame.width, position.v, x, y);
    return {static_cast<float>(x / frame.width), static_cast<float>(y)};
}

std::optional<CorrectedPosition> arraw::correctedFrom(const LensCorrection& correction,
                                                      SensorPoint point) noexcept {
    if (correction.distortion.empty()) {
        return CorrectedPosition{point.u, point.v};
    }
    const Frame frame = frameOf(correction, correction.aspect);
    const double dx = point.u * frame.width - frame.centerX;
    const double dy = point.v - frame.centerY;
    const double target = std::hypot(dx, dy) / frame.reach;
    if (target == 0.0) {
        return CorrectedPosition{correction.centerU, correction.centerV};
    }

    // Solve radius * scale(radius) = target for the radius of the ideal point. Walk outward from
    // the centre to the first interval that reaches the target, which is the ray the lens
    // really maps there when the table is not monotone; within it the table is piecewise linear,
    // so the product is a quadratic that Newton's method finishes in a step or two.
    constexpr double widest = 2.0;
    constexpr int intervals = 512;
    const auto product = [&](double radius) { return radius * distortionAt(correction, radius); };
    double low = 0.0;
    double high = -1.0;
    for (int interval = 1; interval <= intervals; ++interval) {
        const double edge = widest * interval / intervals;
        if (product(edge) >= target) {
            high = edge;
            break;
        }
        low = edge;
    }
    if (high < 0.0) {
        return std::nullopt;
    }
    double radius = std::clamp(target, low, high);
    for (int round = 0; round < 100; ++round) {
        const double value = product(radius) - target;
        if (value > 0.0) {
            high = radius;
        } else {
            low = radius;
        }
        constexpr double step = 1.0e-6;
        const double slope = (product(radius + step) - product(radius - step)) / (2.0 * step);
        double next = slope > 0.0 ? radius - value / slope : 0.5 * (low + high);
        if (!(next > low && next < high)) {
            next = 0.5 * (low + high);
        }
        if (std::abs(next - radius) < 1.0e-13) {
            radius = next;
            break;
        }
        radius = next;
    }
    const double along = radius / target;
    const double ideal_x = frame.centerX + dx * along;
    const double ideal_y = frame.centerY + dy * along;
    const double x = frame.centerX + (ideal_x - frame.centerX) * correction.fill;
    const double y = frame.centerY + (ideal_y - frame.centerY) * correction.fill;
    return CorrectedPosition{x / frame.width, y};
}
