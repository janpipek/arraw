#pragma once

#include <ToneCurveSettings.h>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace arraw::app {

/// @brief One of the four curves of the Tone Curve group.
enum class CurveChannel {
    Luma,  ///< The curve on luminance.
    Red,   ///< The curve on the red channel.
    Green, ///< The curve on the green channel.
    Blue,  ///< The curve on the blue channel.
};

/// @brief Number of ::arraw::app::CurveChannel values.
inline constexpr std::size_t curveChannelCount = 4;

/// @brief Every channel, in the order the editor offers them.
inline constexpr std::array<CurveChannel, curveChannelCount> curveChannels{
    CurveChannel::Luma, CurveChannel::Red, CurveChannel::Green, CurveChannel::Blue};

/// @brief Gives the curve of a channel.
/// @param curves The four curves.
/// @param channel Channel whose curve to give.
/// @return The curve, a member of @p curves.
[[nodiscard]] const ToneCurve& curveOf(const ToneCurveSettings& curves, CurveChannel channel);

/// @copydoc curveOf(const ToneCurveSettings&, CurveChannel)
[[nodiscard]] ToneCurve& curveOf(ToneCurveSettings& curves, CurveChannel channel);

/// @brief Finds the control point nearest a position, if one is close enough.
/// @param curve Curve to search.
/// @param at Position, in the curve's coordinates.
/// @param radius Largest distance that still counts as on the point, in the same coordinates.
/// @return Index of the nearest point within @p radius, or nothing.
[[nodiscard]] std::optional<std::size_t> pointNear(const ToneCurve& curve, CurvePoint at,
                                                   float radius);

/// @brief Checks whether a point can be removed: an interior point of a curve with more than two.
/// @param curve Curve holding the point.
/// @param index Index of the point.
[[nodiscard]] bool isRemovable(const ToneCurve& curve, std::size_t index);

/// @brief Places a point where a drag asks, as near as the curve's invariants allow.
///
/// An end keeps its x and moves only in y; an interior point stays at least
/// ::arraw::minimumCurvePointSpacing from both neighbours, so points never pass
/// each other. y is clamped to 0 to 1.
/// @param curve Well-formed curve holding the point; its neighbours are read.
/// @param index Index of the point.
/// @param target Where the point is asked to go.
/// @return The nearest allowed position.
[[nodiscard]] CurvePoint clampedPosition(const ToneCurve& curve, std::size_t index,
                                         CurvePoint target);

/// @brief Adds a control point, nudged clear of its neighbours if it lands close to one.
///
/// Refuses when the curve is full (::arraw::maximumCurvePoints), when x is not
/// strictly inside 0 to 1, or when the gap the point falls into is too narrow
/// to hold it with ::arraw::minimumCurvePointSpacing on both sides. y is
/// clamped to 0 to 1. The curve stays well formed.
/// @param curve Well-formed curve to add to, changed only on success.
/// @param at Where the point is asked to go.
/// @return Index of the new point, or nothing if it was refused.
std::optional<std::size_t> insertPoint(ToneCurve& curve, CurvePoint at);

/// @brief Removes a control point, unless it is an end or the curve would fall below two points.
/// @param curve Curve to remove from, changed only on success.
/// @param index Index of the point.
/// @return Whether the point was removed.
bool removePoint(ToneCurve& curve, std::size_t index);

/// @brief One drag of one control point, from press to release.
///
/// Keeps the curve as it was when the drag began, so that every step is
/// worked out from that rather than from the step before: a point dragged
/// out of the plot is removed, and brought back it is put back, between the
/// same neighbours.
class CurvePointDrag {
public:
    /// @brief Starts a drag.
    /// @param curve Well-formed curve as the drag begins.
    /// @param index Index of the point dragged.
    CurvePointDrag(ToneCurve curve, std::size_t index);

    /// @brief Gives the curve the drag has reached.
    /// @param target Where the point is asked to go.
    /// @param outside Whether the pointer is out of the plot, which removes a removable point.
    /// @return A well-formed curve.
    [[nodiscard]] ToneCurve curveAt(CurvePoint target, bool outside) const;

    /// @brief Gives the index of the point dragged, in the curve it was dragged in.
    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }

    /// @brief Gives the curve as it was when the drag began.
    [[nodiscard]] const ToneCurve& start() const noexcept {
        return start_;
    }

private:
    ToneCurve start_;
    std::size_t index_;
};

/// @brief Evaluates a curve the way a render does, at evenly spaced inputs.
///
/// Through the engine's own resolution of the curve (ADR 033), so that what a
/// widget draws is what the pixels get.
/// @param curve Well-formed curve.
/// @param count Number of inputs, from 0 to 1 inclusive; at least 2.
/// @return The curve's output at each input.
/// @throws std::invalid_argument if @p curve is not well formed.
[[nodiscard]] std::vector<float> sampleCurve(const ToneCurve& curve, std::size_t count);

} // namespace arraw::app
