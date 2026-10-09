#include "MaskEditing.h"

#include <LocalAdjustmentEdits.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <variant>

namespace arraw::app {

namespace {

constexpr double epsilon = 1e-12;

/// Smallest radius or end distance a drag leaves: clear of the edit rules' own limit.
constexpr double extentFloor = 2.0 * minimumMaskExtent;

double dot(QPointF a, QPointF b) {
    return a.x() * b.x() + a.y() * b.y();
}

double length(QPointF a) {
    return std::hypot(a.x(), a.y());
}

double degrees(double radians) {
    return radians * 180.0 / std::numbers::pi;
}

double radians(double degrees) {
    return degrees * std::numbers::pi / 180.0;
}

QPointF longEdgeOf(const MaskViewMapping& mapping, CorrectedPoint point) {
    const LongEdgePoint at = mapping.frame().longEdgeFrom(point);
    return {at.x, at.y};
}

CorrectedPosition correctedOf(const MaskViewMapping& mapping, QPointF longEdge) {
    return mapping.frame().correctedFromLongEdge({longEdge.x(), longEdge.y()});
}

/// @brief Gives the direction of a screen axis in the long-edge metric (a vector of that frame).
QPointF screenAxis(const MaskViewMapping& mapping, bool horizontal) {
    const QPointF origin = mapping.longEdgeFromWidget({0.0, 0.0});
    return mapping.longEdgeFromWidget(horizontal ? QPointF(1.0, 0.0) : QPointF(0.0, 1.0)) - origin;
}

/// @brief Projects a vector onto whichever screen axis it lies nearer.
QPointF snappedToScreenAxis(QPointF vector, const MaskViewMapping& mapping) {
    const QPointF across = screenAxis(mapping, true);
    const QPointF down = screenAxis(mapping, false);
    const double alongAcross = std::abs(dot(vector, across)) / length(across);
    const double alongDown = std::abs(dot(vector, down)) / length(down);
    const QPointF& axis = alongAcross >= alongDown ? across : down;
    return axis * (dot(vector, axis) / dot(axis, axis));
}

double clampedPosition(double value) {
    return std::clamp(value, static_cast<double>(minimumMaskPosition),
                      static_cast<double>(maximumMaskPosition));
}

CorrectedPosition clampedCorrected(CorrectedPosition at) {
    return {clampedPosition(at.u), clampedPosition(at.v)};
}

/// @brief Passes a shape through the edit rules' normalisation, or gives a fallback.
Mask checked(const Mask& shape, const Mask& fallback) {
    try {
        return normalised(shape);
    } catch (const std::invalid_argument&) {
        return fallback;
    }
}

/// @brief Moves one end of a linear mask away from the other until they are apart.
///
/// @param fixed The end that stays.
/// @param pushed The end that moved, too near.
/// @param original Where the moved end was; it keeps its side of the fixed one.
CorrectedPosition separated(CorrectedPosition fixed, CorrectedPosition pushed,
                            CorrectedPosition original) {
    if (std::hypot(pushed.u - fixed.u, pushed.v - fixed.v) >= extentFloor) {
        return pushed;
    }
    double du = pushed.u - fixed.u;
    double dv = pushed.v - fixed.v;
    const double ou = original.u - fixed.u;
    const double ov = original.v - fixed.v;
    if (std::hypot(du, dv) < epsilon || du * ou + dv * ov < 0.0) {
        du = ou;
        dv = ov;
    }
    const double size = std::hypot(du, dv);
    if (size < epsilon) {
        return {fixed.u, fixed.v + 1.5 * extentFloor};
    }
    return clampedCorrected(
        {fixed.u + du / size * 1.5 * extentFloor, fixed.v + dv / size * 1.5 * extentFloor});
}

Mask draggedLinear(const LinearMask& atPress, MaskHandle handle, QPointF pressLong,
                   QPointF pointerLong, const MaskViewMapping& mapping, bool constrain) {
    const QPointF from = longEdgeOf(mapping, atPress.from);
    const QPointF to = longEdgeOf(mapping, atPress.to);
    const QPointF delta = pointerLong - pressLong;
    QPointF newFrom = from;
    QPointF newTo = to;
    enum { None, From, To } moved = None;
    switch (handle) {
    case MaskHandle::LinearFrom:
        newFrom = from + delta;
        if (constrain) {
            newFrom = to + snappedToScreenAxis(newFrom - to, mapping);
        }
        moved = From;
        break;
    case MaskHandle::LinearTo:
        newTo = to + delta;
        if (constrain) {
            newTo = from + snappedToScreenAxis(newTo - from, mapping);
        }
        moved = To;
        break;
    case MaskHandle::LinearMiddle: {
        // Both ends move by one delta, shortened so that neither passes a limit: clamping each
        // alone would turn the gradient.
        const CorrectedPosition a = correctedOf(mapping, from + delta);
        const CorrectedPosition b = correctedOf(mapping, to + delta);
        const CorrectedPosition a0 = correctedOf(mapping, from);
        const CorrectedPosition b0 = correctedOf(mapping, to);
        double share = 1.0;
        const auto limit = [&share](double start, double end) {
            const double lowest = minimumMaskPosition;
            const double highest = maximumMaskPosition;
            if (end > highest && end > start) {
                share = std::min(share, (highest - start) / (end - start));
            } else if (end < lowest && end < start) {
                share = std::min(share, (lowest - start) / (end - start));
            }
        };
        limit(a0.u, a.u);
        limit(a0.v, a.v);
        limit(b0.u, b.u);
        limit(b0.v, b.v);
        share = std::clamp(share, 0.0, 1.0);
        newFrom = from + delta * share;
        newTo = to + delta * share;
        break;
    }
    case MaskHandle::LinearFromBand:
    case MaskHandle::LinearToBand: {
        const QPointF axis = to - from;
        const double size = length(axis);
        if (size < epsilon) {
            return atPress;
        }
        const QPointF unit = axis / size;
        const QPointF along = unit * dot(delta, unit);
        if (handle == MaskHandle::LinearFromBand) {
            newFrom = from + along;
            moved = From;
        } else {
            newTo = to + along;
            moved = To;
        }
        break;
    }
    default:
        return atPress;
    }
    CorrectedPosition a = clampedCorrected(correctedOf(mapping, newFrom));
    CorrectedPosition b = clampedCorrected(correctedOf(mapping, newTo));
    if (moved == From) {
        a = separated(b, a, atPress.from);
    } else if (moved == To) {
        b = separated(a, b, atPress.to);
    }
    return checked(LinearMask{a.asPoint(), b.asPoint()}, atPress);
}

Mask draggedRadial(const RadialMask& atPress, MaskHandle handle, QPointF pressLong,
                   QPointF pointerLong, const MaskViewMapping& mapping, bool constrain) {
    const QPointF centre = longEdgeOf(mapping, atPress.centre);
    const double angle = radians(atPress.angle);
    const QPointF xAxis(std::cos(angle), std::sin(angle));
    const QPointF yAxis(-std::sin(angle), std::cos(angle));
    RadialMask shape = atPress;
    const auto limitedRadius = [](double radius) {
        return std::clamp(radius, extentFloor, static_cast<double>(maximumMaskRadius));
    };

    switch (handle) {
    case MaskHandle::RadialCentre: {
        shape.centre =
            clampedCorrected(correctedOf(mapping, centre + pointerLong - pressLong)).asPoint();
        break;
    }
    case MaskHandle::RadiusPlusX:
    case MaskHandle::RadiusMinusX:
    case MaskHandle::RadiusPlusY:
    case MaskHandle::RadiusMinusY: {
        const bool onX = handle == MaskHandle::RadiusPlusX || handle == MaskHandle::RadiusMinusX;
        const bool positive =
            handle == MaskHandle::RadiusPlusX || handle == MaskHandle::RadiusPlusY;
        const QPointF axis = (onX ? xAxis : yAxis) * (positive ? 1.0 : -1.0);
        const double radius = onX ? atPress.radiusX : atPress.radiusY;
        const double pressed = dot(pressLong - centre, axis);
        const double now = dot(pointerLong - centre, axis);
        double wanted = radius + (now - pressed);
        if (constrain) {
            const double other = onX ? atPress.radiusY : atPress.radiusX;
            const double low = std::max(extentFloor / radius, extentFloor / other);
            const double high = std::min(maximumMaskRadius / radius, maximumMaskRadius / other);
            const double factor = std::clamp(wanted / radius, low, std::max(low, high));
            shape.radiusX = static_cast<float>(atPress.radiusX * factor);
            shape.radiusY = static_cast<float>(atPress.radiusY * factor);
        } else if (onX) {
            shape.radiusX = static_cast<float>(limitedRadius(wanted));
        } else {
            shape.radiusY = static_cast<float>(limitedRadius(wanted));
        }
        break;
    }
    case MaskHandle::RadialRotation: {
        const QPointF before = pressLong - centre;
        const QPointF after = pointerLong - centre;
        if (length(before) < epsilon || length(after) < epsilon) {
            return atPress;
        }
        const double turn = std::atan2(after.y(), after.x()) - std::atan2(before.y(), before.x());
        shape.angle = wrappedAngle(static_cast<float>(atPress.angle + degrees(turn)));
        break;
    }
    case MaskHandle::RadialFeather: {
        const auto distance = [&](QPointF at) {
            const QPointF q = at - centre;
            return std::hypot(dot(q, xAxis) / atPress.radiusX, dot(q, yAxis) / atPress.radiusY);
        };
        // From where the knob is drawn, which keeps clear of the centre.
        const double drawn = std::max(1.0 - atPress.feather, maskFeatherKnobFloor);
        const double inner = drawn + (distance(pointerLong) - distance(pressLong));
        shape.feather = static_cast<float>(std::clamp(1.0 - inner, 0.0, 1.0));
        break;
    }
    default:
        return atPress;
    }
    return checked(shape, atPress);
}

/// @brief Clips the infinite line through a point to a rectangle.
std::optional<QLineF> clippedLine(QPointF point, QPointF direction, const QRectF& bounds) {
    double first = -std::numeric_limits<double>::infinity();
    double last = std::numeric_limits<double>::infinity();
    const auto clip = [&](double position, double step, double low, double high) {
        if (std::abs(step) < epsilon) {
            return position >= low && position <= high;
        }
        double a = (low - position) / step;
        double b = (high - position) / step;
        if (a > b) {
            std::swap(a, b);
        }
        first = std::max(first, a);
        last = std::min(last, b);
        return true;
    };
    if (!clip(point.x(), direction.x(), bounds.left(), bounds.right()) ||
        !clip(point.y(), direction.y(), bounds.top(), bounds.bottom()) || first > last ||
        !std::isfinite(first) || !std::isfinite(last)) {
        return std::nullopt;
    }
    return QLineF(point + direction * first, point + direction * last);
}

} // namespace

MaskViewMapping::MaskViewMapping(DevelopedFrameMap frame, ViewTransform view,
                                 double devicePixelRatio)
    : frame_(std::move(frame)), view_(std::move(view)), ratio_(devicePixelRatio) {
    if (!(devicePixelRatio > 0.0)) {
        throw std::invalid_argument("The device pixel ratio must be positive");
    }
}

QPointF MaskViewMapping::widgetFrom(CorrectedPosition position) const {
    const DevelopedPoint developed = frame_.developedFrom(position);
    return view_.viewFromFrame({developed.x, developed.y}) / ratio_;
}

CorrectedPosition MaskViewMapping::correctedFrom(QPointF widget) const {
    const QPointF fraction = view_.frameFromView(widget * ratio_);
    return frame_.correctedFrom({fraction.x(), fraction.y()});
}

QPointF MaskViewMapping::widgetFromLongEdge(QPointF position) const {
    return widgetFrom(frame_.correctedFromLongEdge({position.x(), position.y()}));
}

QPointF MaskViewMapping::longEdgeFromWidget(QPointF widget) const {
    const LongEdgePoint at = frame_.longEdgeFrom(correctedFrom(widget));
    return {at.x, at.y};
}

QTransform MaskViewMapping::longEdgeToWidget() const {
    const QPointF origin = widgetFromLongEdge({0.0, 0.0});
    const QPointF across = widgetFromLongEdge({1.0, 0.0}) - origin;
    const QPointF down = widgetFromLongEdge({0.0, 1.0}) - origin;
    return {across.x(), across.y(), down.x(), down.y(), origin.x(), origin.y()};
}

double MaskViewMapping::scale() const {
    const QPointF origin = widgetFromLongEdge({0.0, 0.0});
    return length(widgetFromLongEdge({1.0, 0.0}) - origin);
}

std::vector<HandlePosition> handlePositions(const Mask& mask, const MaskViewMapping& mapping) {
    std::vector<HandlePosition> dots;
    if (const auto* linear = std::get_if<LinearMask>(&mask)) {
        const QPointF from = mapping.widgetFrom(linear->from);
        const QPointF to = mapping.widgetFrom(linear->to);
        dots = {{MaskHandle::LinearFrom, from},
                {MaskHandle::LinearTo, to},
                {MaskHandle::LinearMiddle, (from + to) / 2.0}};
        return dots;
    }
    const auto& radial = std::get<RadialMask>(mask);
    const QPointF centre = longEdgeOf(mapping, radial.centre);
    const double angle = radians(radial.angle);
    const QPointF xAxis(std::cos(angle), std::sin(angle));
    const QPointF yAxis(-std::sin(angle), std::cos(angle));
    const double stem = maskRotationStem / mapping.scale();
    const double inner = std::max(1.0 - radial.feather, maskFeatherKnobFloor);
    const double diagonal = std::numbers::sqrt2 / 2.0;
    const QPointF feather =
        centre + (xAxis * radial.radiusX + yAxis * radial.radiusY) * (inner * diagonal);
    const auto at = [&mapping](MaskHandle handle, QPointF longEdge) {
        return HandlePosition{handle, mapping.widgetFromLongEdge(longEdge)};
    };
    dots = {at(MaskHandle::RadialRotation, centre + xAxis * (radial.radiusX + stem)),
            at(MaskHandle::RadiusPlusX, centre + xAxis * radial.radiusX),
            at(MaskHandle::RadiusMinusX, centre - xAxis * radial.radiusX),
            at(MaskHandle::RadiusPlusY, centre + yAxis * radial.radiusY),
            at(MaskHandle::RadiusMinusY, centre - yAxis * radial.radiusY),
            at(MaskHandle::RadialFeather, feather),
            at(MaskHandle::RadialCentre, centre)};
    return dots;
}

QPointF pinPosition(const Mask& mask, const MaskViewMapping& mapping) {
    if (const auto* linear = std::get_if<LinearMask>(&mask)) {
        return (mapping.widgetFrom(linear->from) + mapping.widgetFrom(linear->to)) / 2.0;
    }
    return mapping.widgetFrom(std::get<RadialMask>(mask).centre);
}

MaskHandle handleAt(const Mask& mask, const MaskViewMapping& mapping, QPointF position,
                    double reach, bool bands) {
    MaskHandle best = MaskHandle::None;
    double bestDistance = reach;
    for (const HandlePosition& dot : handlePositions(mask, mapping)) {
        const double distance = length(dot.position - position);
        if (distance <= reach && (best == MaskHandle::None || distance < bestDistance - 1e-9)) {
            best = dot.handle;
            bestDistance = distance;
        }
    }
    if (best != MaskHandle::None) {
        return best;
    }
    if (const auto* linear = std::get_if<LinearMask>(&mask); linear != nullptr && bands) {
        const QPointF from = mapping.widgetFrom(linear->from);
        const QPointF to = mapping.widgetFrom(linear->to);
        const double size = length(to - from);
        if (size < epsilon) {
            return MaskHandle::None;
        }
        const QPointF unit = (to - from) / size;
        const auto distanceToLine = [&](QPointF through) {
            return std::abs(dot(position - through, unit));
        };
        const double nearFrom = distanceToLine(from);
        const double nearTo = distanceToLine(to);
        if (nearFrom <= reach && nearFrom <= nearTo) {
            return MaskHandle::LinearFromBand;
        }
        if (nearTo <= reach) {
            return MaskHandle::LinearToBand;
        }
    }
    return MaskHandle::None;
}

std::optional<LocalAdjustmentId> pinAt(const DevelopState& state, const MaskViewMapping& mapping,
                                       QPointF position, double reach,
                                       std::optional<LocalAdjustmentId> except) {
    std::optional<LocalAdjustmentId> best;
    double bestDistance = reach;
    for (const LocalAdjustment& adjustment : state.localAdjustments) {
        if (except && *except == adjustment.id) {
            continue;
        }
        const double distance = length(pinPosition(adjustment.shape, mapping) - position);
        if (distance <= reach && (!best || distance <= bestDistance)) {
            best = adjustment.id;
            bestDistance = distance;
        }
    }
    return best;
}

Mask draggedShape(const Mask& atPress, MaskHandle handle, QPointF press, QPointF pointer,
                  const MaskViewMapping& mapping, bool constrain) {
    const QPointF pressLong = mapping.longEdgeFromWidget(press);
    const QPointF pointerLong = mapping.longEdgeFromWidget(pointer);
    if (const auto* linear = std::get_if<LinearMask>(&atPress)) {
        return draggedLinear(*linear, handle, pressLong, pointerLong, mapping, constrain);
    }
    return draggedRadial(std::get<RadialMask>(atPress), handle, pressLong, pointerLong, mapping,
                         constrain);
}

Mask createdShape(MaskTool tool, QPointF press, QPointF pointer, const MaskViewMapping& mapping,
                  QSizeF widget) {
    if (tool == MaskTool::None) {
        throw std::invalid_argument("A mask is created with the Linear or the Radial tool");
    }
    const double side = std::min(widget.width(), widget.height());
    const bool click = length(pointer - press) < maskClickDistance;
    const CorrectedPosition start = mapping.correctedFrom(press);
    if (tool == MaskTool::Linear) {
        const QPointF end = click ? press + QPointF(0.0, side / 5.0) : pointer;
        const CorrectedPosition a = clampedCorrected(start);
        CorrectedPosition b = clampedCorrected(mapping.correctedFrom(end));
        b = separated(a, b, mapping.correctedFrom(press + QPointF(0.0, 1.0)));
        return checked(LinearMask{a.asPoint(), b.asPoint()}, LinearMask{});
    }
    const double radius =
        click ? side / 6.0 / mapping.scale()
              : length(mapping.longEdgeFromWidget(pointer) - mapping.longEdgeFromWidget(press));
    const QPointF across = screenAxis(mapping, true);
    RadialMask shape;
    shape.centre = clampedCorrected(start).asPoint();
    shape.radiusX = shape.radiusY =
        static_cast<float>(std::clamp(radius, extentFloor, static_cast<double>(maximumMaskRadius)));
    shape.angle = wrappedAngle(static_cast<float>(degrees(std::atan2(across.y(), across.x()))));
    shape.feather = 0.5F;
    return checked(shape, RadialMask{});
}

LinearMarks linearMarks(const LinearMask& mask, const MaskViewMapping& mapping,
                        const QRectF& bounds) {
    LinearMarks marks;
    marks.from = mapping.widgetFrom(mask.from);
    marks.to = mapping.widgetFrom(mask.to);
    marks.middle = (marks.from + marks.to) / 2.0;
    const QPointF axis = marks.to - marks.from;
    const double size = length(axis);
    if (size < epsilon) {
        return marks;
    }
    const QPointF across(-axis.y() / size, axis.x() / size);
    marks.fromLine = clippedLine(marks.from, across, bounds);
    marks.toLine = clippedLine(marks.to, across, bounds);
    marks.middleLine = clippedLine(marks.middle, across, bounds);
    return marks;
}

RadialMarks radialMarks(const RadialMask& mask, const MaskViewMapping& mapping) {
    const double angle = radians(mask.angle);
    const QPointF centre = longEdgeOf(mapping, mask.centre);
    const QTransform unitToLongEdge(mask.radiusX * std::cos(angle), mask.radiusX * std::sin(angle),
                                    -mask.radiusY * std::sin(angle), mask.radiusY * std::cos(angle),
                                    centre.x(), centre.y());
    RadialMarks marks;
    marks.unitToWidget = unitToLongEdge * mapping.longEdgeToWidget();
    marks.inner = 1.0 - mask.feather;
    marks.centre = mapping.widgetFromLongEdge(centre);
    return marks;
}

std::optional<LocalAdjustmentId> reconciledSelection(const DevelopState& state,
                                                     std::optional<LocalAdjustmentId> selected) {
    if (selected && findLocalAdjustment(state, *selected) != nullptr) {
        return selected;
    }
    return std::nullopt;
}

} // namespace arraw::app
