#include "CropOverlay.h"

#include <QCursor>
#include <QEasingCurve>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QTransform>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace arraw::app {

namespace {

/// Room around the fitted frame, in logical pixels.
constexpr double margin = 24.0;

/// Distance from a corner or an edge that still grabs it, in logical pixels.
constexpr double grabDistance = 10.0;

/// Length of each arm of a corner bracket, in logical pixels.
constexpr double bracketLength = 16.0;

/// Length of an edge's bar, in logical pixels.
constexpr double edgeBarLength = 16.0;

/// Thickness of the brackets and bars, in logical pixels.
constexpr double handleThickness = 4.0;

/// Shortest line, in logical pixels, that straightens when drawn.
constexpr double shortestLine = 4.0;

/// Darkening of the photograph outside the crop: 60 %.
constexpr int dimAlpha = 153;

/// Lines of the fine grid shown while rotating, across each side.
constexpr int rotationGridLines = 9;

/// Time the view takes to settle after a move or a rotation, in milliseconds.
constexpr int settleMilliseconds = 160;

/// @brief Gives the angle of a vector on screen, in degrees, clockwise positive.
double angleOf(QPointF vector) {
    return std::atan2(vector.y(), vector.x()) * 180.0 / std::numbers::pi;
}

/// @brief Draws the cursor shown outside the frame: a curved, double-headed arrow.
/// @param ratio Device pixels per logical pixel of the screen it shows on.
QCursor drawRotationCursor(double ratio) {
    constexpr int side = 24;
    QPixmap pixmap(QSize(side, side) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath arc;
    const QRectF circle(4.5, 4.5, 15, 15);
    arc.arcMoveTo(circle, 200);
    arc.arcTo(circle, 200, 230);
    const auto draw = [&](const QColor& colour, double width) {
        painter.setPen(QPen(colour, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(arc);
        // Heads at both ends of the arc, pointing along it.
        for (const double degrees : {200.0, 70.0}) {
            const double radians = degrees * std::numbers::pi / 180.0;
            const QPointF tip(12 + 7.5 * std::cos(radians), 12 - 7.5 * std::sin(radians));
            const QPointF along(std::sin(radians), std::cos(radians));
            const double sense = degrees > 100 ? 1.0 : -1.0;
            const QPointF back = tip - sense * along * 4.0;
            const QPointF side(along.y(), -along.x());
            painter.drawLine(tip, back + side * 3.0);
            painter.drawLine(tip, back - side * 3.0);
        }
    };
    draw(Qt::white, 3.5);
    draw(Qt::black, 1.5);
    painter.end();
    // The hot spot is in logical pixels of a pixmap with a pixel ratio.
    return QCursor(pixmap, side / 2, side / 2);
}

/// @brief Gives the turn and flips of a geometry, as a transform of an image of its frame.
QTransform orientationOf(const GeometrySettings& geometry) {
    QTransform turn;
    turn.rotate(90.0 * static_cast<int>(geometry.rotation));
    // Turned first, then flipped (ADR 014); a transform applies the left one first.
    return turn * QTransform::fromScale(geometry.flipHorizontal ? -1.0 : 1.0,
                                        geometry.flipVertical ? -1.0 : 1.0);
}

/// @brief Rounds a logical coordinate to the nearest device pixel.
double snapped(double value, double ratio) {
    return std::round(value * ratio) / ratio;
}

/// @brief Gives the cursor for a handle.
Qt::CursorShape handleCursor(CropHandle handle) {
    switch (handle) {
    case CropHandle::TopLeft:
    case CropHandle::BottomRight:
        return Qt::SizeFDiagCursor;
    case CropHandle::TopRight:
    case CropHandle::BottomLeft:
        return Qt::SizeBDiagCursor;
    case CropHandle::Left:
    case CropHandle::Right:
        return Qt::SizeHorCursor;
    case CropHandle::Top:
    case CropHandle::Bottom:
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}

} // namespace

QImage reorientedImage(const QImage& image, const GeometrySettings& from,
                       const GeometrySettings& to) {
    if (image.isNull() ||
        (from.rotation == to.rotation && from.flipHorizontal == to.flipHorizontal &&
         from.flipVertical == to.flipVertical)) {
        return image;
    }
    // Back from the first geometry's turn and flips, then into the second's. Whole quarter-turns
    // and mirrors map pixels onto pixels, so nothing is resampled.
    return image.transformed(orientationOf(from).inverted() * orientationOf(to));
}

CropOverlay::CropOverlay(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(tr("Crop and straighten"));
    setAccessibleDescription(
        tr("Drag a corner or an edge to resize, inside to move the photograph, outside to "
           "rotate. Ctrl with a drag draws a line to straighten along. Enter accepts, Esc "
           "cancels, O changes the guide, X swaps portrait and landscape."));
    settleAnimation_.setStartValue(0.0);
    settleAnimation_.setEndValue(1.0);
    settleAnimation_.setDuration(settleMilliseconds);
    settleAnimation_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&settleAnimation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                settleProgress_ = value.toDouble();
                update();
            });
    connect(&settleAnimation_, &QVariantAnimation::finished, this, [this] {
        settleFrom_.reset();
        settleProgress_ = 1.0;
        update();
    });
}

void CropOverlay::start(CropEditing editing) {
    reported_ = editing.geometry();
    imageGeometry_ = reported_;
    editing_ = std::move(editing);
    image_ = {};
    pixmap_ = {};
    framed_ = {};
    rendered_ = false;
    drag_ = Drag::None;
    settleAnimation_.stop();
    settleFrom_.reset();
    reportedRenderSize_ = renderSize();
    unsetCursor();
    update();
    emit historyChanged();
}

void CropOverlay::stop() {
    editing_.reset();
    image_ = {};
    pixmap_ = {};
    framed_ = {};
    rendered_ = false;
    drag_ = Drag::None;
    settleAnimation_.stop();
    settleFrom_.reset();
    setStraightening(false);
    releaseMouse();
}

void CropOverlay::setImage(const QImage& image) {
    setImage(image, editing_ ? editing_->geometry() : GeometrySettings{});
}

void CropOverlay::setImage(const QImage& image, const GeometrySettings& renderedFor) {
    if (!editing_) {
        return;
    }
    image_ = reorientedImage(image, renderedFor, editing_->geometry());
    pixmap_ = {};
    imageGeometry_ = editing_->geometry();
    rendered_ = true;
    framed_ = {};
    update();
}

void CropOverlay::setPlaceholder(const QImage& uncropped, const QImage& framed) {
    if (!editing_ || rendered_) {
        return;
    }
    image_ = uncropped;
    pixmap_ = {};
    imageGeometry_ = editing_->geometry();
    framed_ = framed;
    framedGeometry_ = editing_->geometry();
    update();
}

QSize CropOverlay::renderSize() const {
    if (!editing_) {
        return {};
    }
    const CropBox whole = editing_->unstraightened();
    const double scale = std::max(fitted().scale, mapping().scale) * devicePixelRatioF();
    if (!(scale > 0)) {
        return {1, 1};
    }
    // Steps of an eighth of an octave, rounded up, so the render is at most 9% wider than what
    // is shown, and never above the photograph's own pixels. Wider costs paint time (ADR 040).
    const double step = std::min(1.0, std::exp2(std::ceil(std::log2(scale) * 8.0) / 8.0));
    return {std::max(1, static_cast<int>(std::ceil(whole.width * step - 1e-9))),
            std::max(1, static_cast<int>(std::ceil(whole.height * step - 1e-9)))};
}

void CropOverlay::edit(const std::function<void(CropEditing&)>& command) {
    if (!editing_) {
        return;
    }
    editing_->beginStep();
    command(*editing_);
    changed();
    endStep();
}

void CropOverlay::adopt(const GeometrySettings& geometry) {
    if (!editing_) {
        return;
    }
    editing_->beginStep();
    try {
        editing_->adopt(geometry);
    } catch (...) {
        endStep();
        throw;
    }
    reported_ = editing_->geometry();
    changed();
    endStep();
}

void CropOverlay::beginStep() {
    if (editing_) {
        editing_->beginStep();
    }
}

void CropOverlay::endStep() {
    if (editing_) {
        editing_->endStep();
        emit historyChanged();
    }
}

bool CropOverlay::canUndo() const noexcept {
    return editing_ && editing_->canUndo();
}

bool CropOverlay::canRedo() const noexcept {
    return editing_ && editing_->canRedo();
}

void CropOverlay::undo() {
    if (!editing_ || drag_ != Drag::None) {
        return;
    }
    editing_->undo();
    changed();
    emit historyChanged();
}

void CropOverlay::redo() {
    if (!editing_ || drag_ != Drag::None) {
        return;
    }
    editing_->redo();
    changed();
    emit historyChanged();
}

void CropOverlay::setGuide(CropGuide guide) {
    guide_ = guide;
    update();
}

void CropOverlay::cycleGuide() {
    setGuide(static_cast<CropGuide>((static_cast<int>(guide_) + 1) % 4));
}

void CropOverlay::setStraightening(bool straightening) {
    if (straightening == straightening_) {
        return;
    }
    straightening_ = straightening;
    if (straightening_) {
        setCursor(Qt::CrossCursor);
    } else {
        unsetCursor();
    }
    emit straighteningChanged(straightening_);
}

CropOverlay::Mapping CropOverlay::fitted() const {
    if (!editing_) {
        return {};
    }
    const double width = editing_->uprightWidth();
    const double height = editing_->uprightHeight();
    const double roomX = std::max(1.0, this->width() - 2 * margin);
    const double roomY = std::max(1.0, this->height() - 2 * margin);
    const double scale = std::min(roomX / width, roomY / height);
    return {scale, QPointF((this->width() - scale * width) / 2.0,
                           (this->height() - scale * height) / 2.0)};
}

CropOverlay::Mapping CropOverlay::mapping() const {
    if (!editing_) {
        return {};
    }
    const CropBox& crop = editing_->crop();
    switch (drag_) {
    case Drag::Move: {
        // The frame stays where it was; the image moves under it.
        const double scale = pressMapping_.scale;
        const CropPoint centre = crop.centre();
        return {scale, pivot_ - scale * QPointF(centre.x, centre.y)};
    }
    case Drag::Rotate: {
        // The frame's centre stays where it was, at the fitted scale, so the
        // image turns about it and the frame shrinks as it must.
        const double scale = fitted().scale;
        const CropPoint centre = crop.centre();
        return {scale, pivot_ - scale * QPointF(centre.x, centre.y)};
    }
    default:
        break;
    }
    const Mapping target = fitted();
    if (!settleFrom_) {
        return target;
    }
    const double t = settleProgress_;
    return {settleFrom_->scale + (target.scale - settleFrom_->scale) * t,
            settleFrom_->origin + (target.origin - settleFrom_->origin) * t};
}

QPointF CropOverlay::widgetFromUpright(CropPoint point) const {
    const Mapping placement = mapping();
    return placement.origin + placement.scale * QPointF(point.x, point.y);
}

CropPoint CropOverlay::uprightFromWidget(QPointF position) const {
    const Mapping placement = mapping();
    const QPointF upright = (position - placement.origin) / placement.scale;
    return {upright.x(), upright.y()};
}

QRectF CropOverlay::cropRect() const {
    if (!editing_) {
        return {};
    }
    const CropBox& crop = editing_->crop();
    return QRectF(widgetFromUpright({crop.left, crop.top}),
                  widgetFromUpright({crop.right(), crop.bottom()}));
}

CropHit CropOverlay::hitAt(QPointF position) const {
    if (!editing_) {
        return {};
    }
    const QRectF frame = cropRect();
    const CropBox box{frame.left(), frame.top(), frame.width(), frame.height()};
    // Corners first, so a small frame's corner is not taken for an edge. A
    // corner is grabbed along both arms of its bracket and a little around them.
    const double reach = std::min(bracketLength, std::min(frame.width(), frame.height()) / 3.0);
    std::optional<CropHandle> nearest;
    double nearestDistance = std::numeric_limits<double>::infinity();
    for (const CropHandle handle : cropHandles) {
        if (!isCorner(handle)) {
            continue;
        }
        const CropPoint at = handlePosition(box, handle);
        // Distances from the corner into the frame, along each side.
        const double inwardX = (at.x == box.left ? 1.0 : -1.0) * (position.x() - at.x);
        const double inwardY = (at.y == box.top ? 1.0 : -1.0) * (position.y() - at.y);
        const bool onBracket = inwardX >= -grabDistance && inwardY >= -grabDistance &&
                               inwardX <= reach && inwardY <= reach &&
                               std::min(std::abs(inwardX), std::abs(inwardY)) <= grabDistance;
        const double distance = std::hypot(inwardX, inwardY);
        if (onBracket && distance < nearestDistance) {
            nearestDistance = distance;
            nearest = handle;
        }
    }
    if (nearest) {
        return {CropHit::Kind::Handle, *nearest};
    }
    const bool alongX = position.x() >= frame.left() && position.x() <= frame.right();
    const bool alongY = position.y() >= frame.top() && position.y() <= frame.bottom();
    const std::array<std::pair<CropHandle, double>, 4> edges{{
        {CropHandle::Left, alongY ? std::abs(position.x() - frame.left()) : grabDistance + 1},
        {CropHandle::Right, alongY ? std::abs(position.x() - frame.right()) : grabDistance + 1},
        {CropHandle::Top, alongX ? std::abs(position.y() - frame.top()) : grabDistance + 1},
        {CropHandle::Bottom, alongX ? std::abs(position.y() - frame.bottom()) : grabDistance + 1},
    }};
    const auto edge = std::ranges::min_element(
        edges, [](const auto& a, const auto& b) { return a.second < b.second; });
    if (edge->second <= grabDistance) {
        return {CropHit::Kind::Handle, edge->first};
    }
    if (frame.contains(position)) {
        return {CropHit::Kind::Inside, CropHandle::TopLeft};
    }
    return {};
}

void CropOverlay::accept() {
    finish(true);
}

void CropOverlay::reject() {
    finish(false);
}

void CropOverlay::dismiss() {
    if (straightening_) {
        setStraightening(false);
    } else {
        reject();
    }
}

std::vector<QRectF> CropOverlay::handleMarks(CropHandle handle) const {
    if (!editing_) {
        return {};
    }
    const QRectF frame = cropRect();
    const CropBox box{frame.left(), frame.top(), frame.width(), frame.height()};
    const CropPoint at = handlePosition(box, handle);
    const double half = handleThickness / 2.0;
    if (isCorner(handle)) {
        // Two arms from the corner along its sides, as long as a small frame allows.
        const double length =
            std::min(bracketLength, std::min(frame.width(), frame.height()) / 3.0);
        const double sx = at.x == box.left ? 1.0 : -1.0;
        const double sy = at.y == box.top ? 1.0 : -1.0;
        return {QRectF(QPointF(at.x - sx * half, at.y - sy * half),
                       QPointF(at.x + sx * length, at.y + sy * half))
                    .normalized(),
                QRectF(QPointF(at.x - sx * half, at.y - sy * half),
                       QPointF(at.x + sx * half, at.y + sy * length))
                    .normalized()};
    }
    const bool horizontal = handle == CropHandle::Top || handle == CropHandle::Bottom;
    const double length =
        std::min(edgeBarLength, (horizontal ? frame.width() : frame.height()) / 4.0);
    return {horizontal ? QRectF(at.x - length / 2, at.y - half, length, handleThickness)
                       : QRectF(at.x - half, at.y - length / 2, handleThickness, length)};
}

const QCursor& CropOverlay::rotationCursor() {
    const double ratio = devicePixelRatioF();
    if (ratio != rotationCursorRatio_) {
        rotationCursor_ = drawRotationCursor(ratio);
        rotationCursorRatio_ = ratio;
    }
    return rotationCursor_;
}

void CropOverlay::finish(bool accepted) {
    if (!editing_) {
        return;
    }
    stop();
    emit finished(accepted);
}

void CropOverlay::changed() {
    if (!editing_) {
        return;
    }
    const GeometrySettings& geometry = editing_->geometry();
    // A turn or a flip shows at once, from the image there is, until a render
    // of the new geometry arrives.
    const bool reoriented = geometry.rotation != imageGeometry_.rotation ||
                            geometry.flipHorizontal != imageGeometry_.flipHorizontal ||
                            geometry.flipVertical != imageGeometry_.flipVertical;
    if (reoriented) {
        image_ = reorientedImage(image_, imageGeometry_, geometry);
        // Only a new image needs converting again; a drag keeps the pixmap.
        pixmap_ = {};
    }
    imageGeometry_ = geometry;
    if (geometry != framedGeometry_) {
        framed_ = {};
    }
    if (editing_->geometry() != reported_) {
        reported_ = editing_->geometry();
        emit geometryEdited(reported_);
    }
    const QSize wanted = renderSize();
    if (wanted != reportedRenderSize_) {
        reportedRenderSize_ = wanted;
        emit renderWanted();
    }
    update();
}

void CropOverlay::settle(const Mapping& from) {
    settleFrom_ = from;
    settleProgress_ = 0.0;
    settleAnimation_.stop();
    settleAnimation_.start();
}

void CropOverlay::updateCursor(QPointF position, Qt::KeyboardModifiers modifiers) {
    if (!editing_) {
        unsetCursor();
        return;
    }
    if (straightening_ || drag_ == Drag::Line || (modifiers & Qt::ControlModifier)) {
        setCursor(Qt::CrossCursor);
        return;
    }
    const CropHit hit = drag_ == Drag::Resize   ? CropHit{CropHit::Kind::Handle, handle_}
                        : drag_ == Drag::Move   ? CropHit{CropHit::Kind::Inside}
                        : drag_ == Drag::Rotate ? CropHit{}
                                                : hitAt(position);
    switch (hit.kind) {
    case CropHit::Kind::Handle:
        setCursor(handleCursor(hit.handle));
        break;
    case CropHit::Kind::Inside:
        setCursor(drag_ == Drag::Move ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
        break;
    case CropHit::Kind::Outside:
        setCursor(rotationCursor());
        break;
    }
}

bool CropOverlay::event(QEvent* event) {
    // Claimed while cropping, as a spin box claims its keys, so that X does
    // not reject a shot and Esc and Enter reach the frame.
    if (event->type() == QEvent::ShortcutOverride && editing_) {
        auto* key = static_cast<QKeyEvent*>(event);
        const auto modifiers = key->modifiers() & ~Qt::KeypadModifier;
        if (modifiers == Qt::NoModifier &&
            (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
             key->key() == Qt::Key_Escape || key->key() == Qt::Key_O || key->key() == Qt::Key_X)) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void CropOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window).darker(140));
    if (!editing_) {
        return;
    }
    const Mapping placement = mapping();
    const CropBox whole = editing_->unstraightened();
    const QPointF middle =
        widgetFromUpright({editing_->uprightWidth() / 2, editing_->uprightHeight() / 2});
    const double angle = editing_->displayedAngle();

    // The photograph, straightened here rather than by the render (ADR 040).
    // Smooth resampling waits for the end of a rotation drag, so that each frame of it is cheap.
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, drag_ != Drag::Rotate);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(middle);
    painter.rotate(angle);
    const QRectF target(-whole.width * placement.scale / 2, -whole.height * placement.scale / 2,
                        whole.width * placement.scale, whole.height * placement.scale);
    if (image_.isNull()) {
        painter.fillRect(target, palette().color(QPalette::Mid));
    } else {
        if (pixmap_.isNull()) {
            // Converted once: a pixmap is in the surface's own format and paints much faster.
            pixmap_ = QPixmap::fromImage(image_.convertedTo(QImage::Format_RGB32));
        }
        painter.drawPixmap(target, pixmap_, QRectF(pixmap_.rect()));
    }
    painter.restore();

    // Whole device pixels, so the thin line stays thin and sharp at any pixel ratio.
    const double ratio = devicePixelRatioF();
    const QRectF exact = cropRect();
    const QRectF frame(QPointF(snapped(exact.left(), ratio), snapped(exact.top(), ratio)),
                       QPointF(snapped(exact.right(), ratio), snapped(exact.bottom(), ratio)));
    if (!framed_.isNull()) {
        // The frame as shown before the mode: straightened and cropped already, so upright here.
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(exact, framed_);
        painter.restore();
    }
    QPainterPath outside;
    outside.setFillRule(Qt::OddEvenFill);
    outside.addRect(QRectF(rect()));
    outside.addRect(frame);
    painter.fillPath(outside, QColor(0, 0, 0, dimAlpha));

    // Guides while dragging; a fine grid while rotating, to line things up against.
    const auto line = [&](double x0, double y0, double x1, double y1) {
        painter.drawLine(
            QPointF(frame.left() + x0 * frame.width(), frame.top() + y0 * frame.height()),
            QPointF(frame.left() + x1 * frame.width(), frame.top() + y1 * frame.height()));
    };
    if (drag_ == Drag::Rotate) {
        painter.setPen(QPen(QColor(255, 255, 255, 90), 0));
        for (int index = 1; index <= rotationGridLines; ++index) {
            const double at = static_cast<double>(index) / (rotationGridLines + 1);
            line(at, 0, at, 1);
            line(0, at, 1, at);
        }
    } else if (drag_ == Drag::Resize || drag_ == Drag::Move) {
        painter.setPen(QPen(QColor(255, 255, 255, 150), 0));
        switch (guide_) {
        case CropGuide::Thirds:
            for (const double at : {1.0 / 3, 2.0 / 3}) {
                line(at, 0, at, 1);
                line(0, at, 1, at);
            }
            break;
        case CropGuide::Grid:
            for (int index = 1; index < 6; ++index) {
                const double at = index / 6.0;
                line(at, 0, at, 1);
                line(0, at, 1, at);
            }
            break;
        case CropGuide::GoldenRatio: {
            const double minor = 1.0 - 1.0 / std::numbers::phi;
            for (const double at : {minor, 1.0 - minor}) {
                line(at, 0, at, 1);
                line(0, at, 1, at);
            }
            break;
        }
        case CropGuide::Diagonals: {
            // From each corner at 45 degrees on screen, until a side stops it.
            painter.save();
            painter.setClipRect(frame);
            const double reach = frame.width() + frame.height();
            for (const QPointF& corner :
                 {frame.topLeft(), frame.topRight(), frame.bottomLeft(), frame.bottomRight()}) {
                const double sx = corner.x() == frame.left() ? 1.0 : -1.0;
                const double sy = corner.y() == frame.top() ? 1.0 : -1.0;
                painter.drawLine(corner, corner + QPointF(sx * reach, sy * reach));
            }
            painter.restore();
            break;
        }
        }
    }

    // A thin line, one device pixel inside the frame's edge, and thick brackets and bars on
    // it, each with a faint dark rim so they read on a light sky as well.
    painter.setRenderHint(QPainter::Antialiasing, false);
    const double pixel = 1.0 / ratio;
    painter.setPen(Qt::NoPen);
    // Draws a rectangle's outline of a width, inside its right and bottom edges.
    const auto strokeRect = [&](const QRectF& rect, double width, const QColor& colour) {
        const QRectF inner = rect.adjusted(0, 0, -width, -width);
        painter.fillRect(QRectF(inner.left(), inner.top(), rect.width(), width), colour);
        painter.fillRect(QRectF(inner.left(), inner.bottom(), rect.width(), width), colour);
        painter.fillRect(QRectF(inner.left(), inner.top(), width, rect.height()), colour);
        painter.fillRect(QRectF(inner.right(), inner.top(), width, rect.height()), colour);
    };
    strokeRect(frame.adjusted(-pixel, -pixel, pixel, pixel), pixel, QColor(0, 0, 0, 90));
    strokeRect(frame, std::max(1.0, std::round(ratio)) / ratio, QColor(255, 255, 255, 220));
    for (const CropHandle handle : cropHandles) {
        for (const QRectF& mark : handleMarks(handle)) {
            const QRectF snappedMark(
                QPointF(snapped(mark.left(), ratio), snapped(mark.top(), ratio)),
                QPointF(snapped(mark.right(), ratio), snapped(mark.bottom(), ratio)));
            // A rim of one logical pixel keeps the white marks visible on light areas.
            painter.fillRect(snappedMark.adjusted(-1.0, -1.0, 1.0, 1.0), QColor(0, 0, 0, 140));
        }
    }
    for (const CropHandle handle : cropHandles) {
        for (const QRectF& mark : handleMarks(handle)) {
            painter.fillRect(
                QRectF(QPointF(snapped(mark.left(), ratio), snapped(mark.top(), ratio)),
                       QPointF(snapped(mark.right(), ratio), snapped(mark.bottom(), ratio))),
                Qt::white);
        }
    }

    if (drag_ == Drag::Line) {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3));
        painter.drawLine(press_, pointer_);
        painter.setPen(QPen(QColor(255, 210, 0), 1.5));
        painter.drawLine(press_, pointer_);
    }
    if (drag_ == Drag::Rotate) {
        const QString text = tr("%1°").arg(angle, 0, 'f', 1);
        const QRectF label = painter.fontMetrics().boundingRect(text).adjusted(-6, -3, 6, 3);
        const QRectF placed = label.translated(frame.center().x() - label.center().x(),
                                               frame.top() + 8 - label.top());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 160));
        painter.drawRoundedRect(placed, 4, 4);
        painter.setPen(Qt::white);
        painter.drawText(placed, Qt::AlignCenter, text);
    }
}

void CropOverlay::mousePressEvent(QMouseEvent* event) {
    if (!editing_ || event->button() != Qt::LeftButton || drag_ != Drag::None) {
        QWidget::mousePressEvent(event);
        return;
    }
    settleAnimation_.stop();
    settleFrom_.reset();
    press_ = event->position();
    pointer_ = press_;
    pressMapping_ = mapping();
    // The whole drag is one step of the session's history.
    editing_->beginStep();
    if (straightening_ || (event->modifiers() & Qt::ControlModifier)) {
        drag_ = Drag::Line;
    } else {
        const CropHit hit = hitAt(press_);
        editing_->beginGesture();
        const QRectF frame = cropRect();
        pivot_ = frame.center();
        switch (hit.kind) {
        case CropHit::Kind::Handle: {
            drag_ = Drag::Resize;
            handle_ = hit.handle;
            // The handle keeps its distance from the pointer, so grabbing it off centre
            // does not make it jump.
            const CropPoint at = handlePosition(editing_->crop(), hit.handle);
            const CropPoint pointer = uprightFromWidget(press_);
            grabOffset_ = {at.x - pointer.x, at.y - pointer.y};
            break;
        }
        case CropHit::Kind::Inside:
            drag_ = Drag::Move;
            break;
        case CropHit::Kind::Outside:
            drag_ = Drag::Rotate;
            startAngle_ = editing_->displayedAngle();
            startPointerAngle_ = angleOf(press_ - pivot_);
            break;
        }
    }
    updateCursor(press_, event->modifiers());
    update();
    event->accept();
}

void CropOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (!editing_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    pointer_ = event->position();
    switch (drag_) {
    case Drag::None:
        updateCursor(pointer_, event->modifiers());
        break;
    case Drag::Resize: {
        const CropPoint pointer = uprightFromWidget(pointer_);
        editing_->resizeTo(handle_, {pointer.x + grabOffset_.x, pointer.y + grabOffset_.y});
        changed();
        break;
    }
    case Drag::Move: {
        const QPointF delta = (pointer_ - press_) / pressMapping_.scale;
        editing_->moveImageBy(delta.x(), delta.y());
        changed();
        break;
    }
    case Drag::Rotate: {
        // The pointer's turn about the frame's centre, the short way round.
        double turn = angleOf(pointer_ - pivot_) - startPointerAngle_;
        turn = std::remainder(turn, 360.0);
        if (QLineF(pivot_, pointer_).length() > 1.0) {
            editing_->rotateTo(startAngle_ + turn);
            changed();
        }
        break;
    }
    case Drag::Line:
        update();
        break;
    }
    event->accept();
}

void CropOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (!editing_ || event->button() != Qt::LeftButton || drag_ == Drag::None) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    pointer_ = event->position();
    const Mapping during = mapping();
    const Drag ended = drag_;
    drag_ = Drag::None;
    if (ended == Drag::Line) {
        if (QLineF(press_, pointer_).length() >= shortestLine) {
            editing_->straightenAlong(uprightFromWidget(press_), uprightFromWidget(pointer_));
        }
        setStraightening(false);
    }
    if (ended == Drag::Move || ended == Drag::Rotate) {
        settle(during);
    }
    changed();
    endStep();
    updateCursor(pointer_, event->modifiers());
    event->accept();
}

void CropOverlay::mouseDoubleClickEvent(QMouseEvent* event) {
    if (editing_ && event->button() == Qt::LeftButton &&
        hitAt(event->position()).kind == CropHit::Kind::Inside) {
        event->accept();
        accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void CropOverlay::keyPressEvent(QKeyEvent* event) {
    if (!editing_) {
        QWidget::keyPressEvent(event);
        return;
    }
    // R is the window's: its Crop action leaves the mode, keeping the crop.
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        accept();
        break;
    case Qt::Key_Escape:
        dismiss();
        break;
    case Qt::Key_O:
        cycleGuide();
        break;
    case Qt::Key_X:
        edit([](CropEditing& editing) { editing.swapOrientation(); });
        break;
    case Qt::Key_Control:
        updateCursor(mapFromGlobal(QCursor::pos()), event->modifiers());
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void CropOverlay::keyReleaseEvent(QKeyEvent* event) {
    if (editing_ && event->key() == Qt::Key_Control) {
        // Letting go of Ctrl takes the line-drawing cross away without waiting for a move.
        updateCursor(mapFromGlobal(QCursor::pos()), event->modifiers());
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void CropOverlay::wheelEvent(QWheelEvent* event) {
    // The crop mode always fits; a wheel must not zoom the view beneath.
    event->accept();
}

void CropOverlay::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (editing_) {
        changed();
    }
}

} // namespace arraw::app
