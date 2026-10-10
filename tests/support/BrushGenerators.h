#pragma once

#include "BrushRaster.h"
#include "BrushStrokes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace arraw::test {

/// Draws a double in [0, 1) without a library distribution, so a seed means the same everywhere.
inline double unitDouble(std::mt19937_64& g) {
    return static_cast<double>(g() >> 11) * 0x1p-53;
}

/// Draws an integer in [lo, hi].
inline std::uint32_t uniformCount(std::mt19937_64& g, std::uint32_t lo, std::uint32_t hi) {
    return lo + static_cast<std::uint32_t>(g() % (static_cast<std::uint64_t>(hi) - lo + 1));
}

/// Ranges a generated stroke's parameters are drawn from.
struct StrokeStyle {
    float radiusLow;          ///< Smallest radius, long-edge units.
    float radiusHigh;         ///< Largest radius.
    float hardnessLow;        ///< Smallest hardness.
    float hardnessHigh;       ///< Largest hardness.
    float flowLow;            ///< Smallest flow.
    float flowHigh;           ///< Largest flow.
    double eraseChance;       ///< Probability that a stroke erases.
    std::uint32_t pointsLow;  ///< Fewest points.
    std::uint32_t pointsHigh; ///< Most points.
    double stepLow;           ///< Smallest distance between points, long-edge units.
    double stepHigh;          ///< Largest distance between points.
};

inline constexpr StrokeStyle everydayStyle{0.01F, 0.06F, 0.0F, 0.8F,   0.3F, 1.0F,
                                           0.1,   50,    600,  0.0005, 0.002};
inline constexpr StrokeStyle detailStyle{0.002F, 0.01F, 1.0F, 1.0F,   0.5F, 1.0F,
                                         0.0,    20,    200,  0.0003, 0.001};
inline constexpr StrokeStyle washStyle{0.25F, 0.35F, 0.0F, 0.0F,  0.03F, 0.08F,
                                       0.0,   100,   300,  0.002, 0.004};

/// Makes a random walk with inertia: a stroke of the style that wanders about the frame.
///
/// Starts uniformly in [0.05, 0.95] squared (or the box), turns a little at each step, steps
/// the same distance in long-edge units whatever the shape of the frame, and is reflected
/// at the edges of [0, 1] (or the box).
inline Stroke wanderingStroke(std::mt19937_64& g, const StrokeStyle& style, double heightOverWidth,
                              std::optional<std::array<double, 4>> box = {}) {
    const auto between = [&](double lo, double hi) { return lo + (hi - lo) * unitDouble(g); };
    Stroke stroke;
    stroke.radius = static_cast<float>(between(style.radiusLow, style.radiusHigh));
    stroke.hardness = static_cast<float>(between(style.hardnessLow, style.hardnessHigh));
    stroke.flow = static_cast<float>(between(style.flowLow, style.flowHigh));
    stroke.erase = unitDouble(g) < style.eraseChance;
    const std::uint32_t count = uniformCount(g, style.pointsLow, style.pointsHigh);
    const double step = between(style.stepLow, style.stepHigh);
    const std::array<double, 4> bounds = box.value_or(std::array<double, 4>{0.0, 1.0, 0.0, 1.0});
    const std::array<double, 4> start = box.value_or(std::array<double, 4>{0.05, 0.95, 0.05, 0.95});
    double u = between(start[0], start[1]);
    double v = between(start[2], start[3]);
    double dx = between(-1.0, 1.0);
    double dy = between(-1.0, 1.0);
    // Long-edge units to fractions of each side.
    const double alongU = heightOverWidth < 1.0 ? 1.0 : heightOverWidth;
    const double alongV = heightOverWidth < 1.0 ? 1.0 / heightOverWidth : 1.0;
    for (std::uint32_t i = 0; i < count; ++i) {
        stroke.points.push_back({static_cast<float>(u), static_cast<float>(v)});
        dx += 0.3 * between(-1.0, 1.0);
        dy += 0.3 * between(-1.0, 1.0);
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length == 0.0) {
            dx = 1.0;
            dy = 0.0;
        } else {
            dx /= length;
            dy /= length;
        }
        u += dx * step * alongU;
        v += dy * step * alongV;
        if (u < bounds[0]) {
            u = 2 * bounds[0] - u;
            dx = -dx;
        } else if (u > bounds[1]) {
            u = 2 * bounds[1] - u;
            dx = -dx;
        }
        if (v < bounds[2]) {
            v = 2 * bounds[2] - v;
            dy = -dy;
        } else if (v > bounds[3]) {
            v = 2 * bounds[3] - v;
            dy = -dy;
        }
        u = std::clamp(u, bounds[0], bounds[1]);
        v = std::clamp(v, bounds[2], bounds[3]);
    }
    return stroke;
}

/// Makes a stroke with points evenly along a segment.
inline Stroke straightStroke(SensorPoint from, SensorPoint to, std::uint32_t points, float radius,
                             float hardness, float flow, bool erase = false) {
    Stroke stroke{radius, hardness, flow, erase, {}};
    for (std::uint32_t i = 0; i < points; ++i) {
        const float t = points > 1 ? static_cast<float>(i) / static_cast<float>(points - 1) : 0.0F;
        stroke.points.push_back({from.u + (to.u - from.u) * t, from.v + (to.v - from.v) * t});
    }
    return stroke;
}

/// Makes a lawnmower zigzag over [0.05, 0.95] squared, with the given number of points.
inline Stroke zigzagStroke(std::uint32_t points, std::uint32_t rows, float radius, float hardness,
                           float flow) {
    Stroke stroke{radius, hardness, flow, false, {}};
    const std::uint32_t perRow =
        std::max<std::uint32_t>(2, (points + rows - 1) / std::max<std::uint32_t>(rows, 1));
    for (std::uint32_t i = 0; i < points; ++i) {
        const std::uint32_t row = std::min(i / perRow, rows - 1);
        const std::uint32_t along = i - row * perRow;
        const double t = static_cast<double>(along) / static_cast<double>(perRow - 1);
        const double x = 0.05 + 0.9 * (row % 2 == 0 ? t : 1.0 - t);
        const double y = 0.05 + 0.9 * static_cast<double>(row) /
                                    static_cast<double>(std::max<std::uint32_t>(rows - 1, 1));
        stroke.points.push_back(
            {static_cast<float>(std::clamp(x, 0.0, 1.0)), static_cast<float>(y)});
    }
    return stroke;
}

/// Makes a list of wandering strokes, optionally confined to a box (x0, x1, y0, y1).
inline std::shared_ptr<const StrokeList>
paintedMask(std::uint64_t seed, std::uint32_t strokes, const StrokeStyle& style,
            double heightOverWidth, std::optional<std::array<double, 4>> box = {}) {
    std::mt19937_64 g(seed);
    std::vector<Stroke> list;
    for (std::uint32_t i = 0; i < strokes; ++i) {
        list.push_back(wanderingStroke(g, style, heightOverWidth, box));
    }
    return std::make_shared<const StrokeList>(std::move(list));
}

/// Hashes the bits of a plane with FNV-1a.
inline std::uint64_t planeDigest(const CoveragePlane& plane) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const float value : plane.values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (int i = 0; i < 4; ++i) {
            hash ^= (bits >> (8 * i)) & 0xFFU;
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

} // namespace arraw::test
