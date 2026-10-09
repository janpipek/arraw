#pragma once

#include "ViewTransform.h"

#include <DevelopState.h>
#include <DevelopedFrame.h>
#include <LocalAdjustments.h>

#include <QLineF>
#include <QPointF>
#include <QSizeF>
#include <QTransform>

#include <optional>
#include <vector>

namespace arraw::app {

/// @brief Mask kind a creation gesture is armed for.
enum class MaskTool { None, Linear, Radial };

/// @brief A grabbable part of the selected mask's handles (ADR 044, section 10).
enum class MaskHandle {
    None,           ///< Nothing.
    LinearFrom,     ///< The dot at the linear mask's `from`.
    LinearTo,       ///< The dot at the linear mask's `to`.
    LinearMiddle,   ///< The dot midway between the ends.
    LinearFromBand, ///< The line through `from`, away from its dot: moves that end along the axis.
    LinearToBand,   ///< The line through `to`, away from its dot: moves that end along the axis.
    RadialCentre,   ///< The radial mask's centre.
    RadiusPlusX,    ///< The radius point on the positive turned x axis.
    RadiusMinusX,   ///< The radius point on the negative turned x axis.
    RadiusPlusY,    ///< The radius point on the positive turned y axis.
    RadiusMinusY,   ///< The radius point on the negative turned y axis.
    RadialRotation, ///< The knob on a stem beyond the positive x radius point.
    RadialFeather,  ///< The knob on the inner ring, between the positive x and y axes.
};

/// @brief Distance, in logical pixels, within which a handle is grabbed.
inline constexpr double maskHandleReach = 10.0;

/// @brief Length, in logical pixels, of the stem between the x radius point and the rotation knob.
inline constexpr double maskRotationStem = 24.0;

/// @brief Distance, in logical pixels, below which a press and release is a click, not a drag.
inline constexpr double maskClickDistance = 4.0;

/// @brief Smallest elliptical distance the feather knob is drawn at, so it never covers the centre.
inline constexpr double maskFeatherKnobFloor = 0.15;

/// @brief Where the corrected frame lies in a widget: a geometry, a view and a pixel ratio.
///
/// Composes ::arraw::DevelopedFrameMap with ::arraw::app::ViewTransform and the device pixel
/// ratio. The view's frame size is the developed frame in source pixels (the map's own width and
/// height), so the whole chain is a similarity and circles of the long-edge metric are circles
/// on screen. Positions in widgets are logical pixels; the view speaks device pixels.
class MaskViewMapping {
public:
    /// @brief Makes the mapping.
    /// @param frame Placement of the corrected frame in the developed one.
    /// @param view Where the developed frame lies in the view, in device pixels.
    /// @param devicePixelRatio Device pixels per logical pixel; positive.
    MaskViewMapping(DevelopedFrameMap frame, ViewTransform view, double devicePixelRatio);

    /// @brief Gives the widget position of a corrected position.
    [[nodiscard]] QPointF widgetFrom(CorrectedPosition position) const;

    /// @brief Gives the corrected position under a widget position.
    [[nodiscard]] CorrectedPosition correctedFrom(QPointF widget) const;

    /// @brief Gives the widget position of a long-edge position.
    [[nodiscard]] QPointF widgetFromLongEdge(QPointF position) const;

    /// @brief Gives the long-edge position under a widget position.
    [[nodiscard]] QPointF longEdgeFromWidget(QPointF widget) const;

    /// @brief Gives the affine map from the long-edge metric to the widget.
    ///
    /// Maps a circle to a circle, mirrored or not, so it draws an ellipse's unit circle.
    [[nodiscard]] QTransform longEdgeToWidget() const;

    /// @brief Gives the logical pixels one long-edge unit measures on screen.
    [[nodiscard]] double scale() const;

    /// @brief Gives the placement of the corrected frame in the developed one.
    [[nodiscard]] const DevelopedFrameMap& frame() const noexcept {
        return frame_;
    }

    /// @brief Gives the view transform.
    [[nodiscard]] const ViewTransform& view() const noexcept {
        return view_;
    }

    /// @brief Gives the device pixel ratio.
    [[nodiscard]] double devicePixelRatio() const noexcept {
        return ratio_;
    }

private:
    DevelopedFrameMap frame_;
    ViewTransform view_;
    double ratio_;
};

/// @brief A handle and where it is drawn.
struct HandlePosition {
    MaskHandle handle = MaskHandle::None; ///< The handle.
    QPointF position;                     ///< Its dot, in widget logical pixels.
};

/// @brief Gives the dots of a mask's handles, in grab priority order.
///
/// Linear: both ends, then the middle. Radial: the rotation knob, the four radius points, the
/// feather knob, then the centre. The band lines of a linear mask are not dots; see
/// ::arraw::app::linearMarks.
[[nodiscard]] std::vector<HandlePosition> handlePositions(const Mask& mask,
                                                          const MaskViewMapping& mapping);

/// @brief Gives where a mask's pin is drawn: the middle of a linear mask's ends, a radial mask's
/// centre.
[[nodiscard]] QPointF pinPosition(const Mask& mask, const MaskViewMapping& mapping);

/// @brief Finds the handle under a position.
///
/// The nearest dot within @p reach wins, ties in the order of ::arraw::app::handlePositions; if
/// there is none, and @p bands is set, a linear mask's band line within @p reach.
/// @param mask The selected mask.
/// @param mapping Where it is drawn.
/// @param position Pointer position in widget logical pixels.
/// @param reach Distance of grabbing, in logical pixels.
/// @param bands Whether a band line counts; it spans the whole view, so a caller that also
/// offers pins asks for the dots first, then the pins, then the bands.
/// @return The handle, or ::arraw::app::MaskHandle::None.
[[nodiscard]] MaskHandle handleAt(const Mask& mask, const MaskViewMapping& mapping,
                                  QPointF position, double reach = maskHandleReach,
                                  bool bands = true);

/// @brief Finds the pin under a position.
/// @param state State holding the masks.
/// @param mapping Where they are drawn.
/// @param position Pointer position in widget logical pixels.
/// @param reach Distance of grabbing, in logical pixels.
/// @param except Mask whose pin is not offered (the selected one, which has its handles).
/// @return The nearest pin within @p reach, the later mask of two equally near; or nothing.
[[nodiscard]] std::optional<LocalAdjustmentId>
pinAt(const DevelopState& state, const MaskViewMapping& mapping, QPointF position,
      double reach = maskHandleReach, std::optional<LocalAdjustmentId> except = std::nullopt);

/// @brief Gives the shape a handle drag leaves.
///
/// Worked out in the corrected frame's long-edge metric from the shape at the press, so nothing
/// accumulates and a flipped or turned picture needs no special case. The grab offset is kept:
/// the handle does not jump to the pointer. The result is clamped so that it always passes
/// ::arraw::withLocalShape: a linear mask's ends stay apart (an end dragged onto the other, or a
/// hair across it, stops short on its own side; well across, the gradient is reversed), a radius
/// stays within its limits.
/// @param atPress Shape when the button went down.
/// @param handle Handle grabbed; one of another kind leaves the shape as it is.
/// @param press Pointer position at the press, in widget logical pixels.
/// @param pointer Pointer position now.
/// @param mapping Where the mask is drawn.
/// @param constrain Whether Shift is held: an end snaps its direction to horizontal or vertical
/// on screen, a radius point keeps the ratio of the radii.
[[nodiscard]] Mask draggedShape(const Mask& atPress, MaskHandle handle, QPointF press,
                                QPointF pointer, const MaskViewMapping& mapping, bool constrain);

/// @brief Gives the shape a creation gesture makes.
///
/// A drag shorter than ::arraw::app::maskClickDistance is a click and makes the default size at
/// the press: a linear mask runs a fifth of the widget's shorter side straight down on screen, a
/// radial one is a circle of a sixth of it. A longer drag makes a linear mask from the press to
/// the pointer, and a radial circle (x axis horizontal on screen, feather one half) about the
/// press to the pointer. The result passes ::arraw::withLocalAdjustmentAdded.
/// @param tool Linear or Radial.
/// @param press Pointer position at the press, in widget logical pixels.
/// @param pointer Pointer position now.
/// @param mapping Where the mask is drawn.
/// @param widget Size of the widget, in logical pixels.
/// @throws std::invalid_argument if @p tool is None.
[[nodiscard]] Mask createdShape(MaskTool tool, QPointF press, QPointF pointer,
                                const MaskViewMapping& mapping, QSizeF widget);

/// @brief The lines a linear mask draws, in widget logical pixels.
struct LinearMarks {
    QPointF from;                     ///< The dot at `from`.
    QPointF to;                       ///< The dot at `to`.
    QPointF middle;                   ///< The dot midway.
    std::optional<QLineF> fromLine;   ///< Through `from`, weight 1, clipped to the bounds.
    std::optional<QLineF> toLine;     ///< Through `to`, weight 0, clipped to the bounds.
    std::optional<QLineF> middleLine; ///< Through the middle, clipped to the bounds.
};

/// @brief Gives the marks of a linear mask: three parallel lines perpendicular to its axis.
/// @param mask Mask to draw.
/// @param mapping Where it is drawn.
/// @param bounds Rectangle the lines are clipped to, in widget logical pixels.
/// @return The marks; the lines are empty when the ends coincide on screen or a line misses the
/// bounds.
[[nodiscard]] LinearMarks linearMarks(const LinearMask& mask, const MaskViewMapping& mapping,
                                      const QRectF& bounds);

/// @brief What a radial mask draws.
struct RadialMarks {
    QTransform unitToWidget; ///< Takes the unit circle about the origin to the mask's ellipse.
    double inner = 0.0;      ///< Radius of the feather ring in unit circles: `1 - feather`.
    QPointF centre;          ///< The centre, in widget logical pixels.
};

/// @brief Gives the marks of a radial mask.
/// @param mask Mask to draw.
/// @param mapping Where it is drawn.
[[nodiscard]] RadialMarks radialMarks(const RadialMask& mask, const MaskViewMapping& mapping);

/// @brief Keeps a selection while its mask exists.
/// @param state State after a change.
/// @param selected Mask selected before it.
/// @return @p selected if the list still holds that id, else nothing.
[[nodiscard]] std::optional<LocalAdjustmentId>
reconciledSelection(const DevelopState& state, std::optional<LocalAdjustmentId> selected);

} // namespace arraw::app
