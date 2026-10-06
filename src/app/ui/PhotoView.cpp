#include "PhotoView.h"

#include "CropOverlay.h"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QWheelEvent>

#include <cmath>

namespace arraw::app {

namespace {

/// Zoom factor of one wheel notch.
constexpr double wheelStep = 1.15;

/// Angle a wheel notch reports, in eighths of a degree.
constexpr double notchAngle = 120.0;

} // namespace

PhotoView::PhotoView(QWidget* parent) : QWidget(parent) {
    // The view takes the room the window gives it; the image must not decide
    // the window's minimum size, or the photograph could never be fitted smaller.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setMinimumSize(1, 1);
    // Takes the focus when the panel gives it back, and no key of its own: the arrow keys
    // reach the window's actions, which step between photographs.
    setFocusPolicy(Qt::ClickFocus);
    setAutoFillBackground(false);
    crop_ = new CropOverlay(this);
    crop_->hide();
    updateCursor();
}

void PhotoView::setCropMode(bool cropping) {
    if (cropping == isCropMode()) {
        return;
    }
    crop_->setGeometry(rect());
    if (cropping) {
        crop_->show();
        crop_->raise();
        // Whatever gives the view the focus back, such as Enter in a spin box, gives it to
        // the overlay, which claims the mode's keys (ADR 040).
        setFocusProxy(crop_);
        crop_->setFocus();
        return;
    }
    // The view takes the focus before the overlay hides: hiding the focused widget would pass
    // the focus on to the next one in the chain, such as a spin box of the panel, which would
    // then take the keys meant for the window (R, Ctrl+Z).
    const bool focused = crop_->hasFocus();
    setFocusProxy(nullptr);
    if (focused) {
        setFocus();
    }
    crop_->hide();
}

QImage PhotoView::wholeFrameImage() const {
    constexpr double tolerance = 1e-3;
    const bool whole = !image_.isNull() && imageRegion_.left() <= tolerance &&
                       imageRegion_.top() <= tolerance && imageRegion_.right() >= 1.0 - tolerance &&
                       imageRegion_.bottom() >= 1.0 - tolerance;
    return whole ? image_ : background_;
}

bool PhotoView::isCropMode() const {
    return !crop_->isHidden();
}

QSize PhotoView::devicePixels() const {
    return (size() * devicePixelRatioF()).expandedTo({1, 1});
}

ViewTransform PhotoView::transform() const {
    const QSizeF frame(frame_);
    const QSizeF view(devicePixels());
    return fit_ ? ViewTransform::fitted(frame, view) : ViewTransform(frame, view, zoom_, centre_);
}

double PhotoView::zoom() const {
    return transform().zoom();
}

void PhotoView::adopt(const ViewTransform& next, bool fit, bool user) {
    const bool zoomMoved = fit != fit_ || next.zoom() != zoom_;
    const bool moved = zoomMoved || next.centre() != centre_;
    fit_ = fit;
    zoom_ = next.zoom();
    centre_ = next.centre();
    if (moved) {
        update();
    }
    if (zoomMoved) {
        emit zoomChanged();
    }
    if (moved && user) {
        emit viewChanged();
    }
}

void PhotoView::setFrameSize(QSize frame) {
    if (frame == frame_) {
        return;
    }
    frame_ = frame;
    adopt(transform(), fit_, false);
    updateCursor();
}

void PhotoView::setImage(const QImage& image, const QRectF& region, const QImage& background) {
    background_ = background;
    image_ = image;
    imageRegion_ = region;
    updateCursor();
    update();
}

void PhotoView::setBackground(const QImage& background) {
    background_ = background;
    update();
}

void PhotoView::resetView() {
    image_ = {};
    background_ = {};
    update();
    adopt(ViewTransform::fitted(QSizeF(frame_), QSizeF(devicePixels())), true, false);
}

void PhotoView::setPicking(bool picking) {
    picking_ = picking;
    updateCursor();
}

void PhotoView::zoomToFit() {
    adopt(ViewTransform::fitted(QSizeF(frame_), QSizeF(devicePixels())), true, true);
}

void PhotoView::zoomTo(double zoom) {
    const ViewTransform current = transform();
    const QPointF middle(current.view().width() / 2.0, current.view().height() / 2.0);
    const ViewTransform next = current.zoomedAbout(zoom, middle);
    adopt(next, next.isFit(), true);
}

void PhotoView::zoomBy(double factor) {
    zoomTo(transform().zoom() * factor);
}

void PhotoView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    if (image_.isNull() || imageRegion_.isEmpty() || frame_.isEmpty()) {
        return;
    }
    const ViewTransform t = transform();
    const QPointF topLeft = t.viewFromFrame(imageRegion_.topLeft());
    const QPointF bottomRight = t.viewFromFrame(imageRegion_.bottomRight());
    // Whole device pixels at the edges: at 1:1 the render then lands on the
    // screen's pixels, and is not resampled by a fraction of one.
    const double ratio = devicePixelRatioF();
    const QRectF target(
        QPointF(std::round(topLeft.x()) / ratio, std::round(topLeft.y()) / ratio),
        QPointF(std::round(bottomRight.x()) / ratio, std::round(bottomRight.y()) / ratio));
    // Past 1:1 the pixels of the photograph are shown as they are.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, t.zoom() <= 1.0);
    if (!background_.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(
            QRectF(t.viewFromFrame({0.0, 0.0}) / ratio, t.viewFromFrame({1.0, 1.0}) / ratio),
            background_);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, t.zoom() <= 1.0);
    }
    painter.drawImage(target, image_);
}

void PhotoView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    crop_->setGeometry(rect());
    // Fitting follows the size; a zoom is kept, with the centre clamped anew.
    adopt(transform(), fit_, false);
}

void PhotoView::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / notchAngle;
    if (steps == 0.0 || frame_.isEmpty()) {
        event->ignore();
        return;
    }
    const ViewTransform current = transform();
    const ViewTransform next = current.zoomedAbout(current.zoom() * std::pow(wheelStep, steps),
                                                   event->position() * devicePixelRatioF());
    adopt(next, next.isFit(), true);
    event->accept();
}

void PhotoView::mousePressEvent(QMouseEvent* event) {
    const bool panButton = event->button() == Qt::MiddleButton ||
                           (event->button() == Qt::LeftButton &&
                            (!picking_ || (event->modifiers() & Qt::AltModifier)));
    if (event->button() == Qt::LeftButton && picking_ && !panButton) {
        const QPointF point = transform().frameFromView(event->position() * devicePixelRatioF());
        if (!frame_.isEmpty() && point.x() >= 0.0 && point.x() <= 1.0 && point.y() >= 0.0 &&
            point.y() <= 1.0) {
            emit picked(point);
        }
        event->accept();
        return;
    }
    if (panButton && !frame_.isEmpty()) {
        dragging_ = true;
        lastPosition_ = event->position();
        updateCursor();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PhotoView::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPointF delta = (event->position() - lastPosition_) * devicePixelRatioF();
    lastPosition_ = event->position();
    const ViewTransform next = transform().panned(delta);
    adopt(next, fit_, true);
    event->accept();
}

void PhotoView::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) {
        dragging_ = false;
        updateCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PhotoView::updateCursor() {
    if (dragging_) {
        setCursor(Qt::ClosedHandCursor);
    } else if (picking_) {
        setCursor(Qt::CrossCursor);
    } else if (!frame_.isEmpty()) {
        setCursor(Qt::OpenHandCursor);
    } else {
        unsetCursor();
    }
}

} // namespace arraw::app
