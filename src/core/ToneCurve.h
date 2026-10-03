#pragma once

#include <ToneCurveSettings.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace arraw {

/// @brief Number of entries in a resolved tone curve.
///
/// Entry i holds the curve at x = i / (toneCurveSamples - 1), so the first is
/// the curve at 0 and the last at 1. Mirrored by `toneCurveSamples` in
/// `src/gpu/shaders/develop.frag`.
inline constexpr std::size_t toneCurveSamples = 1024;

/// @brief One tone curve of a plan, resolved into a table.
///
/// The plan holds the table rather than the control points, so a tone curve
/// is evaluated by two reads and a blend whatever its shape, and plan equality
/// compares exactly what the pixels will see (ADR 011). A curve that is the
/// identity resolves to a flag that is off and a table of zeros, so that the
/// chain skips it outright and default settings leave every pixel untouched.
struct CurvePlan {
    /// @brief Whether the curve changes anything.
    bool active = false;

    /// @brief The curve sampled over zero to one, linear between entries.
    std::array<float, toneCurveSamples> table{};

    friend bool operator==(const CurvePlan&, const CurvePlan&) = default;
};

/// @brief The four tone curves of a plan, resolved.
struct ToneCurvePlan {
    CurvePlan luma;  ///< Curve on luminance.
    CurvePlan red;   ///< Curve on the red channel.
    CurvePlan green; ///< Curve on the green channel.
    CurvePlan blue;  ///< Curve on the blue channel.

    friend bool operator==(const ToneCurvePlan&, const ToneCurvePlan&) = default;
};

/// @brief Resolves one curve into a table.
///
/// Interpolates the control points with a monotone cubic Hermite spline in
/// the Fritsch--Carlson construction: tangents start as the mean of the
/// neighbouring secants (the end ones as the end secant), are zero at a
/// turning point or beside a flat segment, and are then limited so that every
/// segment is monotone wherever its two points are.
/// A curve drawn rising therefore never dips between its points, and moving
/// one point never makes a neighbouring segment ring.
/// @param curve Control points to resolve.
/// @return The table, or an inactive plan for the identity curve.
/// @throws std::invalid_argument if the curve is not well formed.
[[nodiscard]] CurvePlan curvePlanFor(const ToneCurve& curve);

/// @brief Resolves the four tone curves of the settings.
/// @param settings Curves to resolve.
/// @return The plan, with every identity curve switched off.
/// @throws std::invalid_argument if a curve is not well formed.
[[nodiscard]] ToneCurvePlan toneCurvePlanFor(const ToneCurveSettings& settings);

/// @brief Evaluates a resolved curve at one input.
///
/// Between zero and one the table is blended linearly. Above one the curve
/// continues at slope one from its value at one, `curve(1) + (x - 1)`: headroom
/// above white stays distinct for the highlight shoulder to roll off, offset
/// by what the curve did to white but never stretched or squashed by where its
/// last points happen to sit, and an infinite input stays infinite. Anything
/// not above zero, NaN included, takes the value at zero; the callers in
/// ::arraw::applyToneCurves keep negative and NaN channels away from it.
/// @param plan Resolved curve; it must be active for the result to mean anything.
/// @param x Input in the perceptual coordinate.
/// @return The curve's output in the same coordinate.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline float evaluateCurve(const CurvePlan& plan, float x) {
    constexpr std::size_t last = toneCurveSamples - 1;
    if (!(x > 0.0F)) {
        return plan.table[0];
    }
    if (x >= 1.0F) {
        return plan.table[last] + (x - 1.0F);
    }
    const float position = x * static_cast<float>(last);
    const auto index = std::min(static_cast<std::size_t>(position), last - 1);
    const float fraction = position - static_cast<float>(index);
    return plan.table[index] + fraction * (plan.table[index + 1] - plan.table[index]);
}

} // namespace arraw
