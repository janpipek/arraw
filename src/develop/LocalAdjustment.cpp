#include "develop/LocalAdjustment.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QCoreApplication>

namespace {
constexpr double kPi = 3.14159265358979323846;

// Squared distance between two normalised points in aspect-corrected space.
double aspectDist2(QPointF a, QPointF b, double aspect) {
    const double dx = (a.x() - b.x()) * aspect;
    const double dy = a.y() - b.y();
    return dx * dx + dy * dy;
}
} // namespace

float maskWeight(const LinearMask& m, QPointF uv, float aspect) {
    // Evaluate in aspect-corrected space (x scaled by aspect) so the gradient
    // stays perpendicular to its drawn line on screen. t is a ratio, so the
    // overall scale cancels — only the x:y ratio (= aspect) matters.
    const double a = aspect;
    const double dx = (m.p1.x() - m.p0.x()) * a;
    const double dy = m.p1.y() - m.p0.y();
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 0.0)
        return 0.0f;
    const double px = (uv.x() - m.p0.x()) * a;
    const double py = uv.y() - m.p0.y();
    const double t = std::clamp((px * dx + py * dy) / len2, 0.0, 1.0);
    return static_cast<float>(t * t * (3.0 - 2.0 * t)); // smoothstep falloff
}

LinearHandle nearestHandle(const LinearMask& m, QPointF cursor, float aspect, double pickRadius) {
    const QPointF center = (m.p0 + m.p1) / 2.0;

    const struct {
        LinearHandle handle;
        double dist2;
    } candidates[] = {
        {LinearHandle::P0, aspectDist2(cursor, m.p0, aspect)},
        {LinearHandle::P1, aspectDist2(cursor, m.p1, aspect)},
        {LinearHandle::Center, aspectDist2(cursor, center, aspect)},
    };

    LinearHandle best = LinearHandle::None;
    double bestDist2 = std::numeric_limits<double>::infinity();
    for (const auto& c : candidates) {
        if (c.dist2 < bestDist2) { // strict: endpoints win ties over center
            best = c.handle;
            bestDist2 = c.dist2;
        }
    }
    return bestDist2 <= pickRadius * pickRadius ? best : LinearHandle::None;
}

float radialMaskWeight(const RadialMask& m, QPointF uv, float aspect) {
    // Aspect-correct (x scaled by aspect) so equal radii read as a circle on
    // screen, then un-rotate by the mask angle and normalise by the radii.
    double dx = (uv.x() - m.center.x()) * aspect;
    double dy = uv.y() - m.center.y();
    if (m.angle != 0.0) {
        constexpr double kPi = 3.14159265358979323846;
        const double rad = m.angle * kPi / 180.0;
        const double c = std::cos(rad);
        const double s = std::sin(rad);
        const double rotX = dx * c + dy * s; // rotate by -angle
        const double rotY = -dx * s + dy * c;
        dx = rotX;
        dy = rotY;
    }
    if (m.radiusX <= 0.0 || m.radiusY <= 0.0)
        return 0.0f;
    const double nx = dx / m.radiusX;
    const double ny = dy / m.radiusY;
    const double d = std::sqrt(nx * nx + ny * ny);

    // Weight 1 for d ≤ inner, smoothstep to 0 at the boundary (d = 1).
    const double inner = 1.0 - m.feather;
    double s;
    if (m.feather <= 0.0) {
        s = d >= 1.0 ? 1.0 : 0.0; // hard edge
    } else {
        const double t = std::clamp((d - inner) / m.feather, 0.0, 1.0);
        s = t * t * (3.0 - 2.0 * t);
    }
    double w = 1.0 - s; // affect inside
    if (m.invert)
        w = 1.0 - w;
    return static_cast<float>(w);
}

QPointF radialHandlePos(const RadialMask& m, RadialHandle h, float aspect) {
    const double a = m.angle * kPi / 180.0;
    const double ca = std::cos(a);
    const double sa = std::sin(a);
    switch (h) {
    case RadialHandle::RadiusX:
        // +X axis end (angle+0); x divided by aspect to return to UV space.
        return {m.center.x() + m.radiusX * ca / aspect, m.center.y() + m.radiusX * sa};
    case RadialHandle::RadiusY:
        // +Y axis end (angle+90): direction (-sin a, cos a).
        return {m.center.x() - m.radiusY * sa / aspect, m.center.y() + m.radiusY * ca};
    case RadialHandle::Center:
    default:
        return m.center;
    }
}

RadialMask moveRadialHandle(RadialMask m, RadialHandle h, QPointF to, float aspect) {
    const double dx = (to.x() - m.center.x()) * aspect;
    const double dy = to.y() - m.center.y();
    switch (h) {
    case RadialHandle::Center:
        m.center = to;
        break;
    case RadialHandle::RadiusX:
        m.radiusX = std::sqrt(dx * dx + dy * dy);
        m.angle = std::atan2(dy, dx) * 180.0 / kPi;
        break;
    case RadialHandle::RadiusY: {
        const double a = m.angle * kPi / 180.0;
        m.radiusY = std::abs(-dx * std::sin(a) + dy * std::cos(a)); // onto +Y axis
        break;
    }
    case RadialHandle::None:
        break;
    }
    return m;
}

LinearMask moveHandle(LinearMask m, LinearHandle h, QPointF to) {
    switch (h) {
    case LinearHandle::P0:
        m.p0 = to;
        break;
    case LinearHandle::P1:
        m.p1 = to;
        break;
    case LinearHandle::Center: {
        const QPointF delta = to - (m.p0 + m.p1) / 2.0;
        m.p0 += delta;
        m.p1 += delta;
        break;
    }
    case LinearHandle::None:
        break;
    }
    return m;
}

namespace {
// All local deltas are ±100 except exposure (EV); local temperature is a relative
// ±100 shift, not Kelvin. These two specs match the global sliders' number format.
const FieldSpec kLocalExposureSpec{-500, 500, 0, 0.01f, 0.01f, 2, " EV", true, 0.05f};
const FieldSpec kLocalBipolarSpec{-100, 100, 0, 1.0f, 1.0f, 0, {}, true, 1.0f};

// Untranslated "Linear" / "Radial" / "Brush" for a mask's kind.
const char* maskKindKey(const Mask& mask) {
    if (std::holds_alternative<RadialMask>(mask))
        return "Radial";
    if (std::holds_alternative<BrushMask>(mask))
        return "Brush";
    return "Linear";
}

QString trLocal(const char* text) {
    return QCoreApplication::translate("LocalAdjustment", text);
}
} // namespace

const std::vector<LocalDeltaField>& localDeltaFields() {
    static const std::vector<LocalDeltaField> fields = {
        {"Exposure", kLocalExposureSpec, &LocalAdjustment::exposure},
        {"Contrast", kLocalBipolarSpec, &LocalAdjustment::contrast},
        {"Highlights", kLocalBipolarSpec, &LocalAdjustment::highlights},
        {"Shadows", kLocalBipolarSpec, &LocalAdjustment::shadows},
        {"Whites", kLocalBipolarSpec, &LocalAdjustment::whites},
        {"Blacks", kLocalBipolarSpec, &LocalAdjustment::blacks},
        {"Temp", kLocalBipolarSpec, &LocalAdjustment::temperature},
        {"Tint", kLocalBipolarSpec, &LocalAdjustment::tint},
        {"Saturation", kLocalBipolarSpec, &LocalAdjustment::saturation},
        {"Vibrance", kLocalBipolarSpec, &LocalAdjustment::vibrance},
    };
    return fields;
}

QString maskDisplayName(const std::vector<LocalAdjustment>& list, int index) {
    if (index < 0 || index >= static_cast<int>(list.size()))
        return {};
    const char* kind = maskKindKey(list[index].mask);
    int ordinal = 0; // count same-type masks up to and including this one
    for (int i = 0; i <= index; ++i)
        if (maskKindKey(list[i].mask) == kind)
            ++ordinal;
    return QStringLiteral("%1 %2").arg(trLocal(kind)).arg(ordinal);
}

QString localChangeLabel(
    const std::vector<LocalAdjustment>& before, const std::vector<LocalAdjustment>& after) {
    const QString generic = trLocal("Adjust Local");
    if (before == after)
        return generic;

    // A mask added: the new mask is the first index where the lists diverge.
    if (after.size() == before.size() + 1) {
        size_t i = 0;
        while (i < before.size() && before[i] == after[i])
            ++i;
        return trLocal("Add %1 Mask").arg(trLocal(maskKindKey(after[i].mask)));
    }
    // A mask removed.
    if (before.size() == after.size() + 1) {
        size_t i = 0;
        while (i < after.size() && before[i] == after[i])
            ++i;
        return trLocal("Delete %1 Mask").arg(trLocal(maskKindKey(before[i].mask)));
    }
    if (before.size() != after.size())
        return generic;

    // Same count: name the change only if exactly one mask differs.
    int changed = -1;
    for (size_t i = 0; i < before.size(); ++i)
        if (!(before[i] == after[i])) {
            if (changed != -1)
                return generic;
            changed = static_cast<int>(i);
        }
    if (changed < 0)
        return generic;

    const LocalAdjustment& a = before[changed];
    const LocalAdjustment& b = after[changed];
    const QString name = maskDisplayName(after, changed);

    // The mask itself changed. For a brush that is a paint stroke (the raster
    // pointer swapped); for the parametric kinds it is a handle move.
    if (!(a.mask == b.mask)) {
        if (std::holds_alternative<BrushMask>(b.mask))
            return QStringLiteral("%1 — %2").arg(name, trLocal("Stroke"));
        return trLocal("%1 Geometry").arg(name);
    }

    // A delta field changed → name it with its new value, as the panel shows it.
    // (Each commit touches one field, so the first difference is the edit.)
    for (const LocalDeltaField& f : localDeltaFields())
        if (a.*(f.member) != b.*(f.member)) {
            const QString value = f.spec.format(f.spec.fromParam(b.*(f.member)));
            return QStringLiteral("%1 — %2 %3").arg(name, trLocal(f.label), value);
        }
    return generic;
}
