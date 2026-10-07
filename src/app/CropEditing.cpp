#include "CropEditing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace arraw::app {

namespace {

/// Largest representable distance, for rays that never leave the content.
constexpr double unbounded = std::numeric_limits<double>::infinity();

/// @brief Half-plane a·x + b·y <= c.
struct HalfPlane {
    double a;
    double b;
    double c;

    [[nodiscard]] double excess(CropPoint point) const noexcept {
        return a * point.x + b * point.y - c;
    }
};

/// @brief Gives the half-planes of valid content, with unit normals pointing out of it.
std::vector<HalfPlane> contentPlanes(const CropFrame& frame) {
    const auto& corners = frame.content;
    const CropPoint centre{frame.uprightWidth / 2, frame.uprightHeight / 2};
    std::vector<HalfPlane> planes;
    for (std::size_t index = 0; index < corners.size(); ++index) {
        const auto& from = corners[index];
        const auto& to = corners[(index + 1) % corners.size()];
        double a = to.y - from.y;
        double b = from.x - to.x;
        const double length = std::hypot(a, b);
        a /= length;
        b /= length;
        double c = a * from.x + b * from.y;
        if (a * centre.x + b * centre.y > c) {
            a = -a;
            b = -b;
            c = -c;
        }
        planes.push_back({a, b, c});
    }
    return planes;
}

/// @brief Gives how far a point can travel along a direction before leaving the content.
double exitDistance(const std::vector<HalfPlane>& planes, CropPoint base, CropPoint direction) {
    double distance = unbounded;
    for (const HalfPlane& plane : planes) {
        const double towards = plane.a * direction.x + plane.b * direction.y;
        if (towards > 1e-15) {
            distance = std::min(distance, -plane.excess(base) / towards);
        }
    }
    return std::max(distance, 0.0);
}

/// @brief Projects a point onto the intersection of half-planes, which must not be empty.
/// @return The nearest point that satisfies all of them, or nothing if none was found.
std::optional<CropPoint> project(const std::vector<HalfPlane>& planes, CropPoint wanted,
                                 double slack) {
    const auto feasible = [&](CropPoint point) {
        return std::ranges::all_of(
            planes, [&](const HalfPlane& plane) { return plane.excess(point) <= slack; });
    };
    if (feasible(wanted)) {
        return wanted;
    }
    std::optional<CropPoint> best;
    double bestDistance = unbounded;
    const auto consider = [&](CropPoint point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !feasible(point)) {
            return;
        }
        const double distance = std::hypot(point.x - wanted.x, point.y - wanted.y);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = point;
        }
    };
    // The nearest point of a convex polygon is the point itself, the foot of a
    // perpendicular on an edge, or a vertex: an intersection of two edges' lines.
    for (const HalfPlane& plane : planes) {
        const double norm = plane.a * plane.a + plane.b * plane.b;
        if (norm == 0) {
            continue;
        }
        const double excess = plane.excess(wanted) / norm;
        consider({wanted.x - excess * plane.a, wanted.y - excess * plane.b});
    }
    for (std::size_t i = 0; i < planes.size(); ++i) {
        for (std::size_t j = i + 1; j < planes.size(); ++j) {
            const HalfPlane& p = planes[i];
            const HalfPlane& q = planes[j];
            const double determinant = p.a * q.b - p.b * q.a;
            if (std::abs(determinant) < 1e-12) {
                continue;
            }
            consider(
                {(p.c * q.b - p.b * q.c) / determinant, (p.a * q.c - p.c * q.a) / determinant});
        }
    }
    return best;
}

/// @brief Horizontal direction of a handle from the centre: -1 left, 0 middle, 1 right.
int horizontalOf(CropHandle handle) {
    switch (handle) {
    case CropHandle::TopLeft:
    case CropHandle::Left:
    case CropHandle::BottomLeft:
        return -1;
    case CropHandle::TopRight:
    case CropHandle::Right:
    case CropHandle::BottomRight:
        return 1;
    default:
        return 0;
    }
}

/// @brief Vertical direction of a handle from the centre: -1 top, 0 middle, 1 bottom.
int verticalOf(CropHandle handle) {
    switch (handle) {
    case CropHandle::TopLeft:
    case CropHandle::Top:
    case CropHandle::TopRight:
        return -1;
    case CropHandle::BottomLeft:
    case CropHandle::Bottom:
    case CropHandle::BottomRight:
        return 1;
    default:
        return 0;
    }
}

/// @brief Gives a box spanning two opposite corners.
CropBox spanning(CropPoint a, CropPoint b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
}

} // namespace

bool isCorner(CropHandle handle) noexcept {
    return horizontalOf(handle) != 0 && verticalOf(handle) != 0;
}

CropPoint handlePosition(const CropBox& box, CropHandle handle) noexcept {
    const int sx = horizontalOf(handle);
    const int sy = verticalOf(handle);
    return {sx < 0 ? box.left : (sx > 0 ? box.right() : box.left + box.width / 2),
            sy < 0 ? box.top : (sy > 0 ? box.bottom() : box.top + box.height / 2)};
}

CropEditing::CropEditing(ImageSize sourceSize, ImageOrientation orientation,
                         GeometrySettings geometry)
    : source_{sourceSize, orientation} {
    resolve(std::move(geometry));
    gestureStart_ = geometry_;
}

void CropEditing::resolve(GeometrySettings geometry) {
    // Throws for an invalid geometry before anything changes.
    frame_ = cropFrameFor(source_, geometry);
    geometry_ = std::move(geometry);
}

std::optional<double> CropEditing::lockedRatio() const {
    return arraw::lockedRatio(frame_, geometry_.crop.aspect);
}

double CropEditing::minimumSide() const noexcept {
    return minimumCropFraction *
           std::min(frame_.unstraightened.width, frame_.unstraightened.height);
}

namespace {

/// @brief Stores a box as a geometry's explicit crop, normalised to its frame.
GeometrySettings withBox(GeometrySettings geometry, const CropFrame& frame, const CropBox& box,
                         CropAspect aspect) {
    const auto edge = [](double value, double length) {
        return std::clamp(value / length, 0.0, 1.0);
    };
    geometry.crop.rectangle = UprightCropRect{
        edge(box.left, frame.uprightWidth), edge(box.top, frame.uprightHeight),
        edge(box.right(), frame.uprightWidth), edge(box.bottom(), frame.uprightHeight)};
    geometry.crop.aspect = aspect;
    return geometry;
}

} // namespace

void CropEditing::beginGesture() {
    gestureStart_ = geometry_;
    rotationStart_.reset();
}

void CropEditing::resizeTo(CropHandle handle, CropPoint pointer) {
    rotationStart_.reset();
    const GeometrySettings& start = gestureStart_;
    const CropFrame frame = cropFrameFor(source_, start);
    const CropBox box = frame.crop;
    const auto planes = contentPlanes(frame);
    const double slack = 1e-9 * std::max(frame.uprightWidth, frame.uprightHeight);
    // A crop already below the minimum (a tiny photograph) is not made to grow.
    const double minimum = std::min({minimumSide(), box.width, box.height});
    const double sx = horizontalOf(handle);
    const double sy = verticalOf(handle);
    CropBox next = box;

    const std::optional<double> ratio = arraw::lockedRatio(frame, start.crop.aspect);

    // One free parameter t: corners are base + t * direction, each must stay
    // inside, and t no smaller than the minimum.
    struct Ray {
        CropPoint base;
        CropPoint direction;
    };
    const auto solve = [&](std::initializer_list<Ray> rays, double wanted, double least) {
        double most = unbounded;
        for (const Ray& ray : rays) {
            most = std::min(most, exitDistance(planes, ray.base, ray.direction));
        }
        return std::min(std::max(wanted, least), std::max(most, 0.0));
    };

    if (!ratio && isCorner(handle)) {
        // Two free coordinates: the dragged corner P, with the opposite corner A
        // fixed. P, (Px, Ay) and (Ax, Py) must lie inside the content, which is
        // convex, and the box at least the minimum on each side: an
        // intersection of half-planes, onto which the pointer is projected.
        const CropPoint anchor{sx > 0 ? box.left : box.right(), sy > 0 ? box.top : box.bottom()};
        std::vector<HalfPlane> constraints = planes;
        for (const HalfPlane& plane : planes) {
            if (std::abs(plane.a) > 1e-12) {
                constraints.push_back({plane.a, 0.0, plane.c - plane.b * anchor.y});
            }
            if (std::abs(plane.b) > 1e-12) {
                constraints.push_back({0.0, plane.b, plane.c - plane.a * anchor.x});
            }
        }
        constraints.push_back({-sx, 0.0, -sx * anchor.x - minimum});
        constraints.push_back({0.0, -sy, -sy * anchor.y - minimum});
        const auto corner = project(constraints, pointer, slack);
        if (corner) {
            next = spanning(anchor, *corner);
        }
    } else if (!ratio) {
        // An edge moves along one axis; the two corners on it must stay inside.
        if (sx != 0) {
            const double fixed = sx > 0 ? box.left : box.right();
            const double t =
                solve({{{fixed, box.top}, {sx, 0.0}}, {{fixed, box.bottom()}, {sx, 0.0}}},
                      sx * (pointer.x - fixed), minimum);
            next = spanning({fixed, box.top}, {fixed + sx * t, box.bottom()});
        } else {
            const double fixed = sy > 0 ? box.top : box.bottom();
            const double t =
                solve({{{box.left, fixed}, {0.0, sy}}, {{box.right(), fixed}, {0.0, sy}}},
                      sy * (pointer.y - fixed), minimum);
            next = spanning({box.left, fixed}, {box.right(), fixed + sy * t});
        }
    } else if (isCorner(handle)) {
        // t is the height; the corner travels along the diagonal of the ratio,
        // and the pointer is projected onto that diagonal.
        const double r = *ratio;
        const CropPoint anchor{sx > 0 ? box.left : box.right(), sy > 0 ? box.top : box.bottom()};
        const CropPoint diagonal{sx * r, sy};
        const double wanted =
            ((pointer.x - anchor.x) * diagonal.x + (pointer.y - anchor.y) * diagonal.y) /
            (diagonal.x * diagonal.x + diagonal.y * diagonal.y);
        const double t = solve({{anchor, {sx * r, 0.0}}, {anchor, {0.0, sy}}, {anchor, diagonal}},
                               wanted, std::max(minimum, minimum / r));
        next = spanning(anchor, {anchor.x + sx * r * t, anchor.y + sy * t});
    } else if (sx != 0) {
        // t is the width; the opposite edge stays, its middle fixed, and the
        // height follows the ratio about that middle.
        const double r = *ratio;
        const CropPoint anchor{sx > 0 ? box.left : box.right(), box.top + box.height / 2};
        const double half = 1.0 / (2.0 * r);
        const double t = solve({{anchor, {0.0, half}},
                                {anchor, {0.0, -half}},
                                {anchor, {sx, half}},
                                {anchor, {sx, -half}}},
                               sx * (pointer.x - anchor.x), std::max(minimum, minimum * r));
        next = spanning({anchor.x, anchor.y - t * half}, {anchor.x + sx * t, anchor.y + t * half});
    } else {
        // t is the height, as above with the axes exchanged.
        const double r = *ratio;
        const CropPoint anchor{box.left + box.width / 2, sy > 0 ? box.top : box.bottom()};
        const double half = r / 2.0;
        const double t = solve({{anchor, {half, 0.0}},
                                {anchor, {-half, 0.0}},
                                {anchor, {half, sy}},
                                {anchor, {-half, sy}}},
                               sy * (pointer.y - anchor.y), std::max(minimum, minimum / r));
        next = spanning({anchor.x - t * half, anchor.y}, {anchor.x + t * half, anchor.y + sy * t});
    }
    if (!(next.width > 0) || !(next.height > 0)) {
        return;
    }
    resolve(withBox(start, frame, next, start.crop.aspect));
}

void CropEditing::moveImageBy(double dx, double dy) {
    rotationStart_.reset();
    resolve(withCropMovedBy(source_, gestureStart_, -dx, -dy));
}

void CropEditing::rotateTo(double degrees) {
    rotateStored(storedStraighten(geometry_, degrees));
}

void CropEditing::rotateStored(double straighten) {
    if (!std::isfinite(straighten)) {
        return;
    }
    if (!rotationStart_) {
        rotationStart_ = geometry_;
    }
    resolve(rotatedTo(source_, *rotationStart_, straighten));
}

void CropEditing::straightenAlong(CropPoint from, CropPoint to) {
    if (std::hypot(to.x - from.x, to.y - from.y) <
        1e-9 * std::max(frame_.uprightWidth, frame_.uprightHeight)) {
        return;
    }
    rotationStart_.reset();
    resolve(arraw::straightenedAlong(source_, geometry_, from, to));
}

void CropEditing::setAspect(const CropAspect& aspect) {
    rotationStart_.reset();
    if (const auto* custom = std::get_if<CropRatio>(&aspect); custom && !isWellFormed(*custom)) {
        return;
    }
    resolve(withAspect(source_, geometry_, aspect));
}

void CropEditing::setLocked(bool locked) {
    if (locked == lockedRatio().has_value()) {
        return;
    }
    if (!locked) {
        setAspect(FreeCropAspect{});
        return;
    }
    rotationStart_.reset();
    resolve(withLockedAspect(source_, geometry_));
}

void CropEditing::swapOrientation() {
    rotationStart_.reset();
    resolve(withSwappedOrientation(source_, geometry_));
}

void CropEditing::turn(bool clockwise) {
    rotationStart_.reset();
    resolve(turned(geometry_, clockwise));
}

void CropEditing::flip(bool horizontal) {
    rotationStart_.reset();
    resolve(flipped(geometry_, horizontal));
}

void CropEditing::resetCrop() {
    rotationStart_.reset();
    resolve(withCropReset(geometry_));
}

void CropEditing::adopt(const GeometrySettings& geometry) {
    if (geometry == geometry_) {
        return;
    }
    GeometrySettings sameAngle = geometry;
    sameAngle.straighten = geometry_.straighten;
    if (sameAngle == geometry_) {
        rotateStored(geometry.straighten);
        return;
    }
    // Throws for a geometry that is not valid, before anything changes.
    GeometrySettings next = fittedCrop(source_, geometry);
    rotationStart_.reset();
    resolve(std::move(next));
}

void CropEditing::beginStep() {
    if (stepDepth_++ == 0) {
        stepStart_ = geometry_;
    }
}

void CropEditing::endStep() {
    if (stepDepth_ == 0 || --stepDepth_ > 0) {
        return;
    }
    if (geometry_ != stepStart_) {
        undo_.push_back(stepStart_);
        redo_.clear();
    }
}

bool CropEditing::canUndo() const noexcept {
    return !undo_.empty() || (stepDepth_ > 0 && geometry_ != stepStart_);
}

void CropEditing::undo() {
    if (stepDepth_ > 0) {
        stepDepth_ = 1;
        endStep();
    }
    if (undo_.empty()) {
        return;
    }
    redo_.push_back(geometry_);
    GeometrySettings previous = std::move(undo_.back());
    undo_.pop_back();
    // Every geometry in the history was current once, so valid for the photograph.
    resolve(std::move(previous));
    rotationStart_.reset();
    gestureStart_ = geometry_;
}

void CropEditing::redo() {
    if (stepDepth_ > 0 || redo_.empty()) {
        return;
    }
    undo_.push_back(geometry_);
    GeometrySettings next = std::move(redo_.back());
    redo_.pop_back();
    resolve(std::move(next));
    rotationStart_.reset();
    gestureStart_ = geometry_;
}

} // namespace arraw::app
