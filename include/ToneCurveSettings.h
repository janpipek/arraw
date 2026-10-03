#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace arraw {

/// @brief Fewest control points a tone curve holds: its two ends.
inline constexpr std::size_t minimumCurvePoints = 2;

/// @brief Most control points a tone curve holds, ends included.
inline constexpr std::size_t maximumCurvePoints = 16;

/// @brief Narrowest gap in x between two neighbouring control points.
///
/// A curve resolves into a table of 1024 entries (ADR 033); points closer than
/// a few entries would fall between them, and the curve would no longer pass
/// through them. A hundredth is also about as close as a widget can place two.
inline constexpr float minimumCurvePointSpacing = 0.01F;

/// @brief Rounding slack allowed on an x read from outside: the ends and the spacing.
///
/// An end within this of 0 or 1 is taken as exactly there, and a gap short of
/// ::arraw::minimumCurvePointSpacing by no more than this is wide enough, so
/// that a coordinate written as a decimal and read back as a float still passes.
inline constexpr float curveCoordinateTolerance = 1.0e-6F;

/// @brief Requirements of a well-formed curve, worded for messages.
inline constexpr char toneCurveRequirements[] =
    "2 to 16 points (x, y) from 0 to 1, x at least 0.01 apart, the first at x = 0 and the last "
    "at x = 1";

/// @brief One control point of a tone curve, both coordinates from zero to one.
struct CurvePoint {
    /// @brief Input, in the perceptual coordinate (ADR 010).
    float x = 0.0F;

    /// @brief Output, in the same coordinate.
    float y = 0.0F;

    friend bool operator==(const CurvePoint&, const CurvePoint&) = default;
};

/// @brief A tone curve as the control points the photographer placed.
///
/// A well-formed curve has between ::arraw::minimumCurvePoints and
/// ::arraw::maximumCurvePoints points, every coordinate finite and from zero
/// to one, x increasing by at least ::arraw::minimumCurvePointSpacing (less
/// ::arraw::curveCoordinateTolerance), the first at x = 0 and the last at x = 1.
/// The default is the identity, the straight line through both ends.
struct ToneCurve {
    /// @brief Control points, ordered by increasing x.
    std::vector<CurvePoint> points{{0.0F, 0.0F}, {1.0F, 1.0F}};

    /// @brief Whether the curve is exactly the line from (0, 0) to (1, 1).
    /// @return `true` for the default curve, however it was built.
    [[nodiscard]] bool isIdentity() const {
        return points == std::vector<CurvePoint>{{0.0F, 0.0F}, {1.0F, 1.0F}};
    }

    friend bool operator==(const ToneCurve&, const ToneCurve&) = default;
};

/// @brief Checks whether a curve satisfies the invariants of ::arraw::ToneCurve.
/// @param curve Curve to check.
/// @return `true` if the point count, range, order, spacing and ends are all as required.
[[nodiscard]] bool isWellFormed(const ToneCurve& curve);

/// @brief Puts control points in the order and ends a curve takes.
///
/// Sorts by x (a NaN last) and moves an end within ::arraw::curveCoordinateTolerance
/// of 0 or 1 onto it; nothing else is repaired, so the result may still be
/// malformed. For a frontend that builds a curve piece by piece.
/// @param points Control points, changed in place.
void normaliseCurvePoints(std::vector<CurvePoint>& points);

/// @brief Builds a curve from points given in any order.
///
/// Normalises the points with ::arraw::normaliseCurvePoints and checks the
/// result. The sidecar, the settings document and the command line read a
/// curve through this, so that each accepts the same lists (ADR 033).
/// @param points Control points, in any order.
/// @return The curve, or nothing if it is not well formed once sorted and snapped.
[[nodiscard]] std::optional<ToneCurve> curveFromPoints(std::vector<CurvePoint> points);

/// @brief The tone curves of a photograph, in domain units.
///
/// The luma curve shapes brightness and the colour follows by the ratio; the
/// red, green and blue curves then act on their channels alone, and may shift
/// hue (ADR 011).
struct ToneCurveSettings {
    ToneCurve luma;  ///< Curve on luminance.
    ToneCurve red;   ///< Curve on the red channel.
    ToneCurve green; ///< Curve on the green channel.
    ToneCurve blue;  ///< Curve on the blue channel.

    friend bool operator==(const ToneCurveSettings&, const ToneCurveSettings&) = default;
};

} // namespace arraw
