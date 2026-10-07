#include "CropEditing.h"

#include "GeometryPlan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
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

/// @brief Resolves a geometry's frame alone: its crop removed, so any aspect is accepted.
GeometryPlan frameOf(ImageSize size, ImageOrientation orientation, GeometrySettings geometry) {
    geometry.crop = {};
    return geometryPlanFor(size, orientation, geometry);
}

CropBox toBox(const UprightBox& box) {
    return {box.left, box.top, box.width, box.height};
}

/// @brief Gives the half-planes of valid content, with unit normals pointing out of it.
std::vector<HalfPlane> contentPlanes(const GeometryPlan& frame) {
    const auto corners = contentCorners(frame);
    const UprightPoint centre{frame.uprightWidth / 2, frame.uprightHeight / 2};
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

double displayedStraighten(const GeometrySettings& geometry) noexcept {
    return geometry.flipHorizontal != geometry.flipVertical ? -geometry.straighten
                                                            : geometry.straighten;
}

CropEditing::CropEditing(ImageSize sourceSize, ImageOrientation orientation,
                         GeometrySettings geometry)
    : sourceSize_(sourceSize), orientation_(orientation) {
    resolve(geometry);
    gestureStart_ = geometry_;
}

void CropEditing::resolve(GeometrySettings geometry) {
    // Both throw for an invalid geometry before anything changes.
    const GeometryPlan frame = frameOf(sourceSize_, orientation_, geometry);
    const GeometryPlan full = geometryPlanFor(sourceSize_, orientation_, geometry);
    GeometrySettings level = geometry;
    level.straighten = 0.0;
    const GeometryPlan upright = frameOf(sourceSize_, orientation_, level);
    geometry_ = std::move(geometry);
    uprightWidth_ = frame.uprightWidth;
    uprightHeight_ = frame.uprightHeight;
    crop_ = toBox(full.crop());
    const auto corners = contentCorners(frame);
    for (std::size_t index = 0; index < corners.size(); ++index) {
        content_[index] = {corners[index].x, corners[index].y};
    }
    unstraightened_ = {0.0, 0.0, upright.uprightWidth, upright.uprightHeight};
}

std::optional<double> CropEditing::lockedRatio() const {
    if (std::holds_alternative<OriginalCropAspect>(geometry_.crop.aspect)) {
        return unstraightened_.width / unstraightened_.height;
    }
    if (const auto* ratio = std::get_if<CropRatio>(&geometry_.crop.aspect)) {
        return ratio->widthOverHeight;
    }
    return std::nullopt;
}

double CropEditing::minimumSide() const noexcept {
    return minimumCropFraction * std::min(unstraightened_.width, unstraightened_.height);
}

namespace {

/// @brief Stores a box as a geometry's explicit crop, normalised to its frame.
GeometrySettings withBox(GeometrySettings geometry, const GeometryPlan& frame, const CropBox& box,
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
    const GeometryPlan frame = frameOf(sourceSize_, orientation_, start);
    const CropBox box = toBox(geometryPlanFor(sourceSize_, orientation_, start).crop());
    const auto planes = contentPlanes(frame);
    const double slack = 1e-9 * std::max(frame.uprightWidth, frame.uprightHeight);
    // A crop already below the minimum (a tiny photograph) is not made to grow.
    const double minimum = std::min({minimumSide(), box.width, box.height});
    const double sx = horizontalOf(handle);
    const double sy = verticalOf(handle);
    CropBox next = box;

    std::optional<double> ratio;
    if (std::holds_alternative<OriginalCropAspect>(start.crop.aspect)) {
        GeometrySettings level = start;
        level.straighten = 0.0;
        const GeometryPlan upright = frameOf(sourceSize_, orientation_, level);
        ratio = upright.uprightWidth / upright.uprightHeight;
    } else if (const auto* custom = std::get_if<CropRatio>(&start.crop.aspect)) {
        ratio = custom->widthOverHeight;
    }

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
    const GeometrySettings& start = gestureStart_;
    const GeometryPlan frame = frameOf(sourceSize_, orientation_, start);
    UprightBox box = geometryPlanFor(sourceSize_, orientation_, start).crop();
    box.left -= dx;
    box.top -= dy;
    resolve(withBox(start, frame, toBox(shiftedIntoContent(frame, box)), start.crop.aspect));
}

void CropEditing::rotateTo(double degrees) {
    rotateStored(geometry_.flipHorizontal != geometry_.flipVertical ? -degrees : degrees);
}

void CropEditing::rotateStored(double straighten) {
    if (!std::isfinite(straighten)) {
        return;
    }
    if (!rotationStart_) {
        rotationStart_ = geometry_;
    }
    const GeometrySettings& start = *rotationStart_;
    GeometrySettings next = start;
    next.straighten = std::clamp(straighten, minimumStraighten, maximumStraighten);
    if (start.crop.rectangle) {
        // About the crop's centre (ADR 040): the content under it stays under
        // it, the size is kept, and the fit shrinks about that centre, moving
        // it only when nothing fits there (ADR 014).
        const GeometryPlan before = frameOf(sourceSize_, orientation_, start);
        const UprightBox box = geometryPlanFor(sourceSize_, orientation_, start).crop();
        const GeometryPlan after = frameOf(sourceSize_, orientation_, next);
        const UprightPoint centre =
            after.toUpright(before.toSource({box.left + box.width / 2, box.top + box.height / 2}));
        const UprightBox carried{centre.x - box.width / 2, centre.y - box.height / 2, box.width,
                                 box.height};
        next = withBox(next, after, toBox(fittedToContent(after, carried)), start.crop.aspect);
    }
    resolve(next);
}

void CropEditing::straightenAlong(CropPoint from, CropPoint to) {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    if (std::hypot(dx, dy) < 1e-9 * std::max(uprightWidth_, uprightHeight_)) {
        return;
    }
    // Clockwise on screen, with y down; a line has no direction, so -90 to 90.
    double angle = std::atan2(dy, dx) * 180.0 / std::numbers::pi;
    if (angle > 90.0) {
        angle -= 180.0;
    } else if (angle <= -90.0) {
        angle += 180.0;
    }
    const double turn =
        std::abs(angle) <= 45.0 ? -angle : (angle > 0 ? 90.0 - angle : -90.0 - angle);
    rotationStart_.reset();
    rotateTo(displayedAngle() + turn);
    rotationStart_.reset();
}

void CropEditing::setAspect(const CropAspect& aspect) {
    rotationStart_.reset();
    GeometrySettings next = geometry_;
    next.crop.aspect = aspect;
    std::optional<double> ratio;
    if (std::holds_alternative<OriginalCropAspect>(aspect)) {
        ratio = unstraightened_.width / unstraightened_.height;
    } else if (const auto* custom = std::get_if<CropRatio>(&aspect)) {
        if (!isWellFormed(*custom)) {
            return;
        }
        ratio = custom->widthOverHeight;
    }
    if (ratio && geometry_.crop.rectangle) {
        // The largest box of the ratio inside the present one, about its centre.
        const GeometryPlan frame = frameOf(sourceSize_, orientation_, geometry_);
        const double height = std::min(crop_.height, crop_.width / *ratio);
        const double width = height * *ratio;
        const CropPoint centre = crop_.centre();
        next = withBox(next, frame, {centre.x - width / 2, centre.y - height / 2, width, height},
                       aspect);
    }
    resolve(next);
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
    GeometrySettings next = geometry_;
    if (const auto& rectangle = next.crop.rectangle) {
        // The ratio as the engine checks it, from the stored edges, so it agrees exactly.
        next.crop.aspect = CropRatio{(rectangle->right - rectangle->left) * uprightWidth_ /
                                     ((rectangle->bottom - rectangle->top) * uprightHeight_)};
    } else {
        next.crop.aspect = CropRatio{crop_.width / crop_.height};
    }
    resolve(next);
}

void CropEditing::swapOrientation() {
    rotationStart_.reset();
    GeometrySettings next = geometry_;
    if (const auto ratio = lockedRatio()) {
        // Swapping a swapped Original aspect gives the photograph's own ratio
        // back, so it becomes the Original aspect again.
        const double swappedRatio = 1.0 / *ratio;
        const double own = unstraightened_.width / unstraightened_.height;
        if (std::abs(swappedRatio - own) <= 1e-9 * own) {
            next.crop.aspect = OriginalCropAspect{};
        } else {
            next.crop.aspect = CropRatio{swappedRatio};
        }
        if (!next.crop.rectangle) {
            resolve(next);
            return;
        }
    }
    const GeometryPlan frame = frameOf(sourceSize_, orientation_, geometry_);
    const CropPoint centre = crop_.centre();
    const UprightBox swapped{centre.x - crop_.height / 2, centre.y - crop_.width / 2, crop_.height,
                             crop_.width};
    resolve(withBox(next, frame, toBox(fittedToContent(frame, swapped)), next.crop.aspect));
}

void CropEditing::turn(bool clockwise) {
    rotationStart_.reset();
    GeometrySettings next = geometry_;
    const int steps = (static_cast<int>(next.rotation) + (clockwise ? 1 : 3)) % 4;
    next.rotation = static_cast<QuarterTurn>(steps);
    // A turn after a flip is the flip of the other axis after the turn.
    std::swap(next.flipHorizontal, next.flipVertical);
    if (auto& rectangle = next.crop.rectangle) {
        const UprightCropRect r = *rectangle;
        rectangle = clockwise ? UprightCropRect{1 - r.bottom, r.left, 1 - r.top, r.right}
                              : UprightCropRect{r.top, 1 - r.right, r.bottom, 1 - r.left};
    }
    if (auto* ratio = std::get_if<CropRatio>(&next.crop.aspect)) {
        ratio->widthOverHeight = 1.0 / ratio->widthOverHeight;
    }
    resolve(next);
}

void CropEditing::flip(bool horizontal) {
    rotationStart_.reset();
    GeometrySettings next = geometry_;
    auto& rectangle = next.crop.rectangle;
    if (horizontal) {
        next.flipHorizontal = !next.flipHorizontal;
        if (rectangle) {
            rectangle = UprightCropRect{1 - rectangle->right, rectangle->top, 1 - rectangle->left,
                                        rectangle->bottom};
        }
    } else {
        next.flipVertical = !next.flipVertical;
        if (rectangle) {
            rectangle = UprightCropRect{rectangle->left, 1 - rectangle->bottom, rectangle->right,
                                        1 - rectangle->top};
        }
    }
    resolve(next);
}

void CropEditing::resetCrop() {
    rotationStart_.reset();
    GeometrySettings next = geometry_;
    next.crop.rectangle.reset();
    resolve(next);
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
    const GeometryPlan frame = frameOf(sourceSize_, orientation_, geometry);
    GeometrySettings next = geometry;
    if (next.crop.rectangle) {
        // Throws for a rectangle that is not well formed or disagrees with its aspect.
        const UprightBox box = geometryPlanFor(sourceSize_, orientation_, next).crop();
        const auto& r = *next.crop.rectangle;
        const UprightBox stored{r.left * frame.uprightWidth, r.top * frame.uprightHeight,
                                (r.right - r.left) * frame.uprightWidth,
                                (r.bottom - r.top) * frame.uprightHeight};
        if (!isInsideContent(frame, stored)) {
            next = withBox(next, frame, toBox(box), next.crop.aspect);
        }
    }
    rotationStart_.reset();
    resolve(next);
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
