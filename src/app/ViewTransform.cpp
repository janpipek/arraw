#include "ViewTransform.h"

#include <algorithm>
#include <cmath>

namespace arraw::app {

namespace {

/// Tolerance of preset matching: half a percentage point.
constexpr double presetTolerance = 0.005;

/// Slack when snapping fractions of a pixel to whole ones, against rounding error.
constexpr double snapSlack = 1e-6;

bool empty(QSizeF size) {
    return !(size.width() > 0.0) || !(size.height() > 0.0);
}

/// @brief Clamps a centre coordinate for one axis.
/// @param centre Wanted centre, in fractions of the frame.
/// @param frame Frame length on this axis.
/// @param view View length on this axis.
/// @param zoom Device pixels per frame pixel.
double clampAxis(double centre, double frame, double view, double zoom) {
    if (!(frame > 0.0)) {
        return 0.5;
    }
    const double extent = view / (zoom * frame); // The visible fraction.
    if (extent >= 1.0) {
        return 0.5;
    }
    return std::clamp(centre, extent / 2.0, 1.0 - extent / 2.0);
}

double clampZoom(double zoom, QSizeF frame, QSizeF view) {
    if (std::isnan(zoom)) {
        zoom = 1.0;
    }
    return std::clamp(zoom, ViewTransform::minZoom(frame, view), maxZoom);
}

} // namespace

int matchingZoomPreset(double zoom) {
    for (std::size_t i = 0; i < zoomPresets.size(); ++i) {
        if (std::abs(zoom - zoomPresets[i]) <= presetTolerance) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QString zoomLabel(double zoom, bool fit) {
    if (fit) {
        return QStringLiteral("Fit");
    }
    if (zoom == 1.0) {
        return QStringLiteral("1:1");
    }
    return zoomPercentLabel(zoom);
}

QString zoomPercentLabel(double zoom) {
    return QStringLiteral("%1%").arg(std::lround(zoom * 100.0));
}

ViewTransform::ViewTransform(QSizeF frame, QSizeF view, double zoom, QPointF centre)
    : frame_(frame), view_(view), zoom_(clampZoom(zoom, frame, view)),
      centre_(clampAxis(centre.x(), frame.width(), view.width(), zoom_),
              clampAxis(centre.y(), frame.height(), view.height(), zoom_)) {}

ViewTransform ViewTransform::fitted(QSizeF frame, QSizeF view) {
    return ViewTransform(frame, view, fitZoom(frame, view));
}

double ViewTransform::fitZoom(QSizeF frame, QSizeF view) {
    if (empty(frame) || empty(view)) {
        return 1.0;
    }
    return std::min({1.0, view.width() / frame.width(), view.height() / frame.height()});
}

double ViewTransform::minZoom(QSizeF frame, QSizeF view) {
    return std::min(fitZoom(frame, view), 0.25);
}

bool ViewTransform::isFit() const {
    return std::abs(zoom_ - fitZoom(frame_, view_)) <= 1e-9;
}

QPointF ViewTransform::frameFromView(QPointF position) const {
    if (empty(frame_)) {
        return centre_;
    }
    return {centre_.x() + (position.x() - view_.width() / 2.0) / (zoom_ * frame_.width()),
            centre_.y() + (position.y() - view_.height() / 2.0) / (zoom_ * frame_.height())};
}

QPointF ViewTransform::viewFromFrame(QPointF point) const {
    return {view_.width() / 2.0 + (point.x() - centre_.x()) * zoom_ * frame_.width(),
            view_.height() / 2.0 + (point.y() - centre_.y()) * zoom_ * frame_.height()};
}

ViewTransform ViewTransform::zoomedAbout(double zoom, QPointF position) const {
    const double next = clampZoom(zoom, frame_, view_);
    if (empty(frame_)) {
        return ViewTransform(frame_, view_, next, centre_);
    }
    const QPointF kept = frameFromView(position);
    const QPointF centre(kept.x() - (position.x() - view_.width() / 2.0) / (next * frame_.width()),
                         kept.y() -
                             (position.y() - view_.height() / 2.0) / (next * frame_.height()));
    return ViewTransform(frame_, view_, next, centre);
}

ViewTransform ViewTransform::panned(QPointF delta) const {
    if (empty(frame_)) {
        return *this;
    }
    return ViewTransform(frame_, view_, zoom_,
                         {centre_.x() - delta.x() / (zoom_ * frame_.width()),
                          centre_.y() - delta.y() / (zoom_ * frame_.height())});
}

QRectF ViewTransform::visibleRegion() const {
    if (empty(frame_)) {
        return {0.0, 0.0, 1.0, 1.0};
    }
    const double halfWidth = view_.width() / (2.0 * zoom_ * frame_.width());
    const double halfHeight = view_.height() / (2.0 * zoom_ * frame_.height());
    const double left = std::clamp(centre_.x() - halfWidth, 0.0, 1.0);
    const double right = std::clamp(centre_.x() + halfWidth, 0.0, 1.0);
    const double top = std::clamp(centre_.y() - halfHeight, 0.0, 1.0);
    const double bottom = std::clamp(centre_.y() + halfHeight, 0.0, 1.0);
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

QRect ViewTransform::visiblePixels() const {
    if (empty(frame_)) {
        return {};
    }
    const QRectF region = visibleRegion();
    const int width = static_cast<int>(std::lround(frame_.width()));
    const int height = static_cast<int>(std::lround(frame_.height()));
    const auto edges = [](double from, double to, int length) {
        const int first =
            std::clamp(static_cast<int>(std::floor(from * length + snapSlack)), 0, length - 1);
        const int last =
            std::clamp(static_cast<int>(std::ceil(to * length - snapSlack)), first + 1, length);
        return std::pair{first, last};
    };
    const auto [x0, x1] = edges(region.left(), region.right(), width);
    const auto [y0, y1] = edges(region.top(), region.bottom(), height);
    return QRect(x0, y0, x1 - x0, y1 - y0);
}

QSize ViewTransform::outputSize() const {
    const QRect pixels = visiblePixels();
    const double scale = std::min(zoom_, 1.0);
    return {std::max(1, static_cast<int>(std::lround(pixels.width() * scale))),
            std::max(1, static_cast<int>(std::lround(pixels.height() * scale)))};
}

} // namespace arraw::app
