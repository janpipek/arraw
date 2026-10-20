#include "BrushPainting.h"

#include <DevelopedFrame.h>
#include <LocalAdjustmentEdits.h>

#include <QCoreApplication>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <variant>

namespace arraw::app {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("arraw::app::BrushPainting", text);
}

/// @brief Reads a float of the store, clamped, or the fallback when it is missing or not a number.
float storedFloat(QSettings& store, const QString& key, float fallback, float low, float high) {
    bool ok = false;
    const double value = store.value(key).toDouble(&ok);
    if (!ok || !std::isfinite(value)) {
        return fallback;
    }
    return static_cast<float>(
        std::clamp(value, static_cast<double>(low), static_cast<double>(high)));
}

/// @brief Finds a brush of a state.
const BrushMask* brushOf(const DevelopState& state, LocalAdjustmentId id) {
    const LocalAdjustment* adjustment = findLocalAdjustment(state, id);
    return adjustment == nullptr ? nullptr : std::get_if<BrushMask>(&adjustment->shape);
}

/// @brief Tells which cap or budget a mask holding some strokes would pass by one more.
BrushRefusal refusalFor(std::size_t strokes, const StrokeBudget& held, const StrokeBudget& cost) {
    if (strokes + 1 > maximumStrokesPerMask) {
        return BrushRefusal::Strokes;
    }
    if (cost.points > maximumStrokePoints || held.points + cost.points > maximumPointsPerMask) {
        return BrushRefusal::Points;
    }
    if (held.sweptArea + cost.sweptArea > maximumSweptAreaPerMask) {
        return BrushRefusal::SweptArea;
    }
    if (held.dabs + cost.dabs > maximumDabsPerMask) {
        return BrushRefusal::Dabs;
    }
    return BrushRefusal::None;
}

/// @brief Clamps a point to the positions the core stores.
SensorPoint clampedPoint(SensorPoint point) noexcept {
    point.u = std::clamp(point.u, minimumMaskPosition, maximumMaskPosition);
    point.v = std::clamp(point.v, minimumMaskPosition, maximumMaskPosition);
    return point;
}

double distance(SensorPoint a, SensorPoint b) noexcept {
    const double du = static_cast<double>(a.u) - b.u;
    const double dv = static_cast<double>(a.v) - b.v;
    return std::sqrt(du * du + dv * dv);
}

} // namespace

BrushSettings restoreBrushSettings(QSettings& store) {
    const BrushSettings defaults;
    BrushSettings settings;
    settings.radius =
        storedFloat(store, "brush/radius", defaults.radius, minimumBrushRadius, maximumBrushRadius);
    settings.hardness = storedFloat(store, "brush/hardness", defaults.hardness, 0.0F, 1.0F);
    settings.flow = storedFloat(store, "brush/flow", defaults.flow, minimumBrushFlow, 1.0F);
    return settings;
}

void saveBrushSettings(const BrushSettings& settings, QSettings& store) {
    store.setValue("brush/radius", static_cast<double>(settings.radius));
    store.setValue("brush/hardness", static_cast<double>(settings.hardness));
    store.setValue("brush/flow", static_cast<double>(settings.flow));
}

BrushSettings resized(BrushSettings settings, int steps) {
    const double radius = static_cast<double>(settings.radius) * std::pow(brushResizeFactor, steps);
    settings.radius = static_cast<float>(std::clamp(radius, static_cast<double>(minimumBrushRadius),
                                                    static_cast<double>(maximumBrushRadius)));
    return settings;
}

BrushSettings hardened(BrushSettings settings, int steps) {
    // Rounded to a thousandth so that repeated steps do not drift (0.70000005).
    const double hardness = settings.hardness + static_cast<double>(brushHardnessStep) * steps;
    settings.hardness =
        static_cast<float>(std::clamp(std::round(hardness * 1000.0) / 1000.0, 0.0, 1.0));
    return settings;
}

BrushRefusal refusalOf(const DevelopState& state, LocalAdjustmentId id, const Stroke& stroke) {
    const BrushMask* brush = brushOf(state, id);
    if (brush == nullptr) {
        return BrushRefusal::NotABrush;
    }
    if (brush->strokes == nullptr) {
        return refusalFor(0, StrokeBudget{}, budgetOf(stroke));
    }
    const StrokeList& list = *brush->strokes;
    if (list.rasteriser() != brushRasteriserVersion) {
        return BrushRefusal::NotABrush;
    }
    return refusalFor(list.size(), list.budget(), budgetOf(stroke));
}

Stroke probeStroke(const BrushSettings& settings, SensorPoint at) {
    // One radius along u: the budgets measure lengths in normalised units, which bound the
    // long-edge ones for every aspect. Step back from the right edge so the length is kept.
    const float end = at.u + settings.radius <= maximumMaskPosition ? at.u + settings.radius
                                                                    : at.u - settings.radius;
    return Stroke{
        settings.radius, settings.hardness, settings.flow, settings.erase, {at, {end, at.v}}};
}

QString describeRefusal(BrushRefusal refusal, const QString& maskName, bool midStroke) {
    switch (refusal) {
    case BrushRefusal::None:
    case BrushRefusal::NotABrush:
        return {};
    case BrushRefusal::MasksFull:
        return tr("A photograph holds at most %1 masks.").arg(maximumLocalAdjustments);
    case BrushRefusal::Strokes:
    case BrushRefusal::Points:
    case BrushRefusal::SweptArea:
    case BrushRefusal::Dabs:
        return midStroke ? tr("%1 is full: the stroke stops here.").arg(maskName)
                         : tr("%1 is full. Paint on a new brush.").arg(maskName);
    }
    return {};
}

std::optional<LiveStroke> LiveStroke::begin(const DevelopState& baseline,
                                            std::optional<LocalAdjustmentId> target,
                                            const BrushSettings& settings, bool erase,
                                            QPointF press, const MaskViewMapping& mapping,
                                            BrushRefusal& refused) {
    refused = BrushRefusal::None;
    LiveStroke stroke;
    stroke.base_ = baseline;
    if (target) {
        stroke.id_ = *target;
    } else {
        if (!canAddLocalAdjustment(baseline)) {
            refused = BrushRefusal::MasksFull;
            return std::nullopt;
        }
        stroke.base_ = withLocalAdjustmentAdded(baseline, Mask{BrushMask{}});
        stroke.id_ = stroke.base_.localAdjustments.back().id;
    }
    // The style and positions the core will store, so that the checks below measure the same
    // numbers.
    BrushSettings captured = settings;
    captured.radius = std::clamp(settings.radius, minimumBrushRadius, maximumBrushRadius);
    captured.hardness = std::clamp(settings.hardness, 0.0F, 1.0F);
    captured.flow = std::clamp(settings.flow, 0.0F, 1.0F);
    captured.erase = erase;
    const CorrectedPosition corrected = mapping.correctedFrom(press);
    const SensorPoint at = clampedPoint(mapping.sensorFrom(corrected));
    refused = refusalOf(stroke.base_, stroke.id_, probeStroke(captured, at));
    if (refused != BrushRefusal::None) {
        return std::nullopt;
    }
    if (const BrushMask* brush = brushOf(stroke.base_, stroke.id_); brush->strokes != nullptr) {
        stroke.baseBudget_ = brush->strokes->budget();
        stroke.baseStrokes_ = brush->strokes->size();
    }
    stroke.live_ = Stroke{captured.radius, captured.hardness, captured.flow, erase, {at}};
    const LongEdgePoint longEdge = mapping.frame().longEdgeFrom(corrected);
    stroke.lastKept_ = QPointF(longEdge.x, longEdge.y);
    return stroke;
}

const DevelopState& LiveStroke::state() const {
    if (stateStale_) {
        state_ = withStrokeAppended(base_, id_, live_);
        stateStale_ = false;
    }
    return state_;
}

bool LiveStroke::extend(QPointF pointer, const MaskViewMapping& mapping, bool release) {
    if (refusal_ != BrushRefusal::None) {
        return false;
    }
    const CorrectedPosition corrected = mapping.correctedFrom(pointer);
    const LongEdgePoint longEdge = mapping.frame().longEdgeFrom(corrected);
    const QPointF here(longEdge.x, longEdge.y);
    const double gap = std::hypot(here.x() - lastKept_.x(), here.y() - lastKept_.y());
    const double spacing = std::max(1.0 / mapping.scale(), 0.1 * static_cast<double>(live_.radius));
    if (release ? gap == 0.0 : gap < spacing) {
        return false;
    }
    const SensorPoint point = clampedPoint(mapping.sensorFrom(corrected));
    const SensorPoint last = live_.points.back();
    if (point == last) {
        return false;
    }

    // Constant work per move: the live part's cost is tracked, not re-measured.
    const double step = distance(last, point);
    const bool split = atPointCap();
    StrokeBudget held = baseBudget_;
    std::size_t strokes = baseStrokes_;
    std::size_t points = live_.points.size() + 1;
    double length = liveLength_ + step;
    if (split) {
        held += budgetOf(live_.points.size(), liveLength_, live_.radius);
        ++strokes;
        points = 2;
        length = step;
    }
    const BrushRefusal refusal = refusalFor(strokes, held, budgetOf(points, length, live_.radius));
    if (refusal != BrushRefusal::None) {
        refusal_ = refusal;
        return false;
    }
    if (split) {
        // The finished stroke is the last one of state(), the state last reported when the caller
        // flushed first; the new stroke starts at its last point.
        base_ = state();
        baseBudget_ = held;
        baseStrokes_ = strokes;
        live_.points = {last};
    }
    live_.points.push_back(point);
    liveLength_ = length;
    lastKept_ = here;
    stateStale_ = true;
    reported_ = false;
    return true;
}

} // namespace arraw::app
