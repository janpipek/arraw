#include "CropGeometry.h"

#include "GeometryPlan.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <variant>

namespace arraw {

namespace {

/// @brief Resolves a geometry's frame alone: its crop removed, so any aspect is accepted.
GeometryPlan frameOf(SourceShape source, GeometrySettings geometry) {
    geometry.crop = {};
    return geometryPlanFor(source.size, source.orientation, geometry);
}

CropBox toBox(const UprightBox& box) {
    return {box.left, box.top, box.width, box.height};
}

/// @brief Stores a box as a geometry's explicit crop, normalised to its frame.
GeometrySettings withBox(GeometrySettings geometry, double uprightWidth, double uprightHeight,
                         const CropBox& box, CropAspect aspect) {
    const auto edge = [](double value, double length) {
        return std::clamp(value / length, 0.0, 1.0);
    };
    geometry.crop.rectangle =
        UprightCropRect{edge(box.left, uprightWidth), edge(box.top, uprightHeight),
                        edge(box.right(), uprightWidth), edge(box.bottom(), uprightHeight)};
    geometry.crop.aspect = std::move(aspect);
    return geometry;
}

/// @brief Turns a normalised rectangle by a quarter on screen.
UprightCropRect turnedRectangle(const UprightCropRect& r, bool clockwise) {
    return clockwise ? UprightCropRect{1 - r.bottom, r.left, 1 - r.top, r.right}
                     : UprightCropRect{r.top, 1 - r.right, r.bottom, 1 - r.left};
}

void reciprocate(CropAspect& aspect) {
    if (auto* ratio = std::get_if<CropRatio>(&aspect)) {
        ratio->widthOverHeight = 1.0 / ratio->widthOverHeight;
    }
}

} // namespace

CropFrame cropFrameFor(SourceShape source, const GeometrySettings& geometry) {
    // Each throws for an invalid geometry.
    const GeometryPlan frame = frameOf(source, geometry);
    const GeometryPlan full = geometryPlanFor(source.size, source.orientation, geometry);
    GeometrySettings level = geometry;
    level.straighten = 0.0;
    const GeometryPlan upright = frameOf(source, level);
    CropFrame result;
    result.uprightWidth = frame.uprightWidth;
    result.uprightHeight = frame.uprightHeight;
    result.crop = toBox(full.crop());
    const auto corners = contentCorners(frame);
    for (std::size_t index = 0; index < corners.size(); ++index) {
        result.content[index] = {corners[index].x, corners[index].y};
    }
    result.unstraightened = {0.0, 0.0, upright.uprightWidth, upright.uprightHeight};
    return result;
}

std::optional<double> lockedRatio(const CropFrame& frame, const CropAspect& aspect) {
    if (std::holds_alternative<OriginalCropAspect>(aspect)) {
        return frame.unstraightened.width / frame.unstraightened.height;
    }
    if (const auto* ratio = std::get_if<CropRatio>(&aspect)) {
        if (!isWellFormed(*ratio)) {
            throw std::invalid_argument("crop aspect ratio must be positive and finite");
        }
        return ratio->widthOverHeight;
    }
    return std::nullopt;
}

double displayedStraighten(const GeometrySettings& geometry) noexcept {
    return geometry.flipHorizontal != geometry.flipVertical ? -geometry.straighten
                                                            : geometry.straighten;
}

double storedStraighten(const GeometrySettings& geometry, double displayed) noexcept {
    return geometry.flipHorizontal != geometry.flipVertical ? -displayed : displayed;
}

GeometrySettings turned(GeometrySettings geometry, bool clockwise) noexcept {
    const int steps = (static_cast<int>(geometry.rotation) + (clockwise ? 1 : 3)) % 4;
    geometry.rotation = static_cast<QuarterTurn>(steps);
    // A turn after a flip is the flip of the other axis after the turn.
    std::swap(geometry.flipHorizontal, geometry.flipVertical);
    if (auto& rectangle = geometry.crop.rectangle) {
        rectangle = turnedRectangle(*rectangle, clockwise);
    }
    reciprocate(geometry.crop.aspect);
    return geometry;
}

GeometrySettings flipped(GeometrySettings geometry, bool horizontal) noexcept {
    auto& rectangle = geometry.crop.rectangle;
    if (horizontal) {
        geometry.flipHorizontal = !geometry.flipHorizontal;
        if (rectangle) {
            rectangle = UprightCropRect{1 - rectangle->right, rectangle->top, 1 - rectangle->left,
                                        rectangle->bottom};
        }
    } else {
        geometry.flipVertical = !geometry.flipVertical;
        if (rectangle) {
            rectangle = UprightCropRect{rectangle->left, 1 - rectangle->bottom, rectangle->right,
                                        1 - rectangle->top};
        }
    }
    return geometry;
}

GeometrySettings withRotation(GeometrySettings geometry, QuarterTurn rotation) noexcept {
    const int steps = (static_cast<int>(rotation) - static_cast<int>(geometry.rotation) + 4) % 4;
    geometry.rotation = rotation;
    // With the flips kept, a stored clockwise step is a clockwise turn on screen
    // when both or neither flip is set, and an anticlockwise one when one is.
    const bool clockwise = geometry.flipHorizontal == geometry.flipVertical;
    for (int step = 0; step < steps; ++step) {
        if (auto& rectangle = geometry.crop.rectangle) {
            rectangle = turnedRectangle(*rectangle, clockwise);
        }
    }
    if (steps % 2 != 0) {
        reciprocate(geometry.crop.aspect);
    }
    return geometry;
}

GeometrySettings rotatedTo(SourceShape source, const GeometrySettings& baseline,
                           double straighten) {
    if (!std::isfinite(straighten)) {
        throw std::invalid_argument("straighten must be finite");
    }
    // Throws for a baseline that is not valid.
    const GeometryPlan full = geometryPlanFor(source.size, source.orientation, baseline);
    GeometrySettings next = baseline;
    next.straighten = std::clamp(straighten, minimumStraighten, maximumStraighten);
    if (baseline.crop.rectangle) {
        // About the crop's centre (ADR 040): the content under it stays under
        // it, the size is kept, and the fit shrinks about that centre, moving
        // it only when nothing fits there (ADR 014).
        const GeometryPlan before = frameOf(source, baseline);
        const UprightBox box = full.crop();
        const GeometryPlan after = frameOf(source, next);
        const UprightPoint centre =
            after.toUpright(before.toSource({box.left + box.width / 2, box.top + box.height / 2}));
        const UprightBox carried{centre.x - box.width / 2, centre.y - box.height / 2, box.width,
                                 box.height};
        next = withBox(next, after.uprightWidth, after.uprightHeight,
                       toBox(fittedToContent(after, carried)), baseline.crop.aspect);
    }
    return next;
}

GeometrySettings straightenedAlong(SourceShape source, const GeometrySettings& geometry,
                                   CropPoint from, CropPoint to) {
    const CropFrame frame = cropFrameFor(source, geometry);
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    if (!(std::hypot(dx, dy) >= 1e-9 * std::max(frame.uprightWidth, frame.uprightHeight))) {
        return geometry;
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
    return rotatedTo(source, geometry,
                     storedStraighten(geometry, displayedStraighten(geometry) + turn));
}

GeometrySettings withAspect(SourceShape source, GeometrySettings geometry,
                            const CropAspect& aspect) {
    const CropFrame frame = cropFrameFor(source, geometry);
    const auto ratio = lockedRatio(frame, aspect);
    geometry.crop.aspect = aspect;
    if (ratio && geometry.crop.rectangle) {
        // The largest box of the ratio inside the present one, about its centre.
        const double height = std::min(frame.crop.height, frame.crop.width / *ratio);
        const double width = height * *ratio;
        const CropPoint centre = frame.crop.centre();
        geometry = withBox(std::move(geometry), frame.uprightWidth, frame.uprightHeight,
                           {centre.x - width / 2, centre.y - height / 2, width, height}, aspect);
    }
    return geometry;
}

GeometrySettings withLockedAspect(SourceShape source, GeometrySettings geometry) {
    const CropFrame frame = cropFrameFor(source, geometry);
    if (lockedRatio(frame, geometry.crop.aspect)) {
        return geometry;
    }
    if (const auto& rectangle = geometry.crop.rectangle) {
        // The ratio as the engine checks it, from the stored edges, so it agrees exactly.
        geometry.crop.aspect =
            CropRatio{(rectangle->right - rectangle->left) * frame.uprightWidth /
                      ((rectangle->bottom - rectangle->top) * frame.uprightHeight)};
    } else {
        geometry.crop.aspect = CropRatio{frame.crop.width / frame.crop.height};
    }
    return geometry;
}

GeometrySettings withSwappedOrientation(SourceShape source, GeometrySettings geometry) {
    const CropFrame frame = cropFrameFor(source, geometry);
    if (const auto ratio = lockedRatio(frame, geometry.crop.aspect)) {
        // Swapping a swapped Original aspect gives the photograph's own ratio
        // back, so it becomes the Original aspect again.
        const double swappedRatio = 1.0 / *ratio;
        const double own = frame.unstraightened.width / frame.unstraightened.height;
        if (std::abs(swappedRatio - own) <= 1e-9 * own) {
            geometry.crop.aspect = OriginalCropAspect{};
        } else {
            geometry.crop.aspect = CropRatio{swappedRatio};
        }
        if (!geometry.crop.rectangle) {
            return geometry;
        }
    }
    const GeometryPlan plane = frameOf(source, geometry);
    const CropPoint centre = frame.crop.centre();
    const UprightBox swapped{centre.x - frame.crop.height / 2, centre.y - frame.crop.width / 2,
                             frame.crop.height, frame.crop.width};
    CropAspect aspect = geometry.crop.aspect;
    return withBox(std::move(geometry), frame.uprightWidth, frame.uprightHeight,
                   toBox(fittedToContent(plane, swapped)), std::move(aspect));
}

GeometrySettings withCropReset(GeometrySettings geometry) noexcept {
    geometry.crop.rectangle.reset();
    return geometry;
}

GeometrySettings withCropMovedBy(SourceShape source, GeometrySettings geometry, double dx,
                                 double dy) {
    const GeometryPlan frame = frameOf(source, geometry);
    UprightBox box = geometryPlanFor(source.size, source.orientation, geometry).crop();
    box.left += dx;
    box.top += dy;
    CropAspect aspect = geometry.crop.aspect;
    return withBox(std::move(geometry), frame.uprightWidth, frame.uprightHeight,
                   toBox(shiftedIntoContent(frame, box)), std::move(aspect));
}

GeometrySettings fittedCrop(SourceShape source, GeometrySettings geometry) {
    const GeometryPlan frame = frameOf(source, geometry);
    // Throws for a rectangle that is not well formed or disagrees with its aspect.
    const UprightBox box = geometryPlanFor(source.size, source.orientation, geometry).crop();
    if (geometry.crop.rectangle) {
        const auto& r = *geometry.crop.rectangle;
        const UprightBox stored{r.left * frame.uprightWidth, r.top * frame.uprightHeight,
                                (r.right - r.left) * frame.uprightWidth,
                                (r.bottom - r.top) * frame.uprightHeight};
        if (!isInsideContent(frame, stored)) {
            CropAspect aspect = geometry.crop.aspect;
            geometry = withBox(std::move(geometry), frame.uprightWidth, frame.uprightHeight,
                               toBox(box), std::move(aspect));
        }
    }
    return geometry;
}

} // namespace arraw
