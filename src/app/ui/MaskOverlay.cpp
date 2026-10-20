#include "MaskOverlay.h"

#include "MaskPresentation.h"
#include "PhotoView.h"
#include "TimingTrace.h"
#include "ViewTransform.h"

#include <DevelopedFrame.h>
#include <LocalAdjustmentEdits.h>
#include <SettingDescriptors.h>

#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <exception>
#include <variant>

namespace arraw::app {

namespace {

/// Radius of a handle's dot, in logical pixels.
constexpr double dotRadius = 5.0;

/// Radius of a pin's ring, in logical pixels.
constexpr double pinRadius = 5.0;

/// Cells of the tint grid per logical pixel of the widget, on each axis.
constexpr double coverageDensity = 0.5;

/// Largest number of cells of the tint grid.
constexpr double coverageCells = 1048576.0;

/// Duration of a tint above which a drag thins the grid, in milliseconds.
constexpr double slowCoverageMilliseconds = 8.0;

/// Opacity of the tint where the weight is 1.
constexpr double tintOpacity = 0.5;

QColor markColour(int alpha = 255) {
    return {245, 245, 245, alpha};
}

QColor rimColour(int alpha = 255) {
    return {20, 20, 20, std::min(alpha, 170)};
}

/// @brief Strokes a line white over a dark rim, so that it reads on a light sky and a dark one.
void strokeLine(QPainter& painter, const QLineF& line, double width, Qt::PenStyle style,
                int alpha = 255) {
    painter.setPen(QPen(rimColour(alpha), width + 2.0));
    painter.drawLine(line);
    QPen pen(markColour(alpha), width);
    pen.setStyle(style);
    painter.setPen(pen);
    painter.drawLine(line);
}

/// @brief Strokes a unit-circle ellipse in the painter's transform, with the same rim.
void strokeUnitCircle(QPainter& painter, double radius, Qt::PenStyle style) {
    QPen rim(rimColour(), 3.0);
    rim.setCosmetic(true);
    painter.setPen(rim);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(0.0, 0.0), radius, radius);
    QPen pen(markColour(), 1.0);
    pen.setCosmetic(true);
    pen.setStyle(style);
    painter.setPen(pen);
    painter.drawEllipse(QPointF(0.0, 0.0), radius, radius);
}

void drawDot(QPainter& painter, QPointF at) {
    painter.setPen(QPen(rimColour(), 1.5));
    painter.setBrush(markColour());
    painter.drawEllipse(at, dotRadius, dotRadius);
}

void drawPin(QPainter& painter, QPointF at, bool enabled) {
    const int alpha = enabled ? 255 : 110;
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(rimColour(alpha), 3.5));
    painter.drawEllipse(at, pinRadius, pinRadius);
    painter.setPen(QPen(markColour(alpha), 1.5));
    painter.drawEllipse(at, pinRadius, pinRadius);
}

/// Radius of a brush's ring, in logical pixels, below which a cross stands in for it.
constexpr double crossBelowRadius = 4.0;

/// Radius of a brush's ring, in logical pixels, below which the ring is not drawn at all.
constexpr double ringBelowRadius = 1.5;

/// Length of the cross that stands for a small brush, in logical pixels.
constexpr double crossLength = 9.0;

/// Length of the bar that marks an erasing brush, in logical pixels.
constexpr double eraseBarLength = 8.0;

/// Room the ring's strokes need beyond its radius, in logical pixels.
constexpr double ringMargin = 4.0;

/// @brief Strokes a circle white over a dark rim, in widget pixels.
void strokeCircle(QPainter& painter, QPointF centre, double radius, Qt::PenStyle style) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(rimColour(), 3.0));
    painter.drawEllipse(centre, radius, radius);
    QPen pen(markColour(), 1.0);
    pen.setStyle(style);
    painter.setPen(pen);
    painter.drawEllipse(centre, radius, radius);
}

/// @brief Tells whether a key is one the mode claims.
bool claimedKey(int key) {
    return key == Qt::Key_Escape || key == Qt::Key_O || key == Qt::Key_Delete ||
           key == Qt::Key_Backspace || key == Qt::Key_Space;
}

} // namespace

MaskOverlay::MaskOverlay(PhotoView* view) : QWidget(view), view_(view) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_NoSystemBackground);
    setAccessibleName(tr("Masks"));
    setAccessibleDescription(
        tr("Drag a handle to move or reshape the selected mask; with Linear or Radial armed, "
           "drag on the photograph to draw one. Esc cancels, O shows where the mask applies, "
           "Delete removes it."));
    strokeTimer_.setSingleShot(true);
    strokeTimer_.setInterval(0);
    connect(&strokeTimer_, &QTimer::timeout, this, &MaskOverlay::reportStroke);
    coverageTimer_.setSingleShot(true);
    coverageTimer_.setInterval(0);
    connect(&coverageTimer_, &QTimer::timeout, this, &MaskOverlay::updateCoverage);
    connect(view_, &PhotoView::transformChanged, this, [this] {
        scheduleCoverage();
        update();
    });
    hide();
}

void MaskOverlay::setScene(const DevelopState& state, const std::optional<SourceShape>& source) {
    state_ = state;
    source_ = source;
    frame_.reset();
    if (source_) {
        try {
            frame_.emplace(*source_, state_.settings.geometry);
        } catch (const std::exception&) {
            // A geometry the renderer will report on: nothing to place a mask in.
        }
    }
    selection_ = reconciledSelection(state_, selection_);
    scheduleCoverage();
    refreshBrushFull();
    refresh(false);
}

void MaskOverlay::setLens(const LensCorrection& lens) {
    if ((lens_ == nullptr && !lens.active()) || (lens_ != nullptr && *lens_ == lens)) {
        return;
    }
    lens_ = lens.active() ? std::make_shared<const LensCorrection>(lens) : nullptr;
    scheduleCoverage();
    update();
}

void MaskOverlay::setSelection(std::optional<LocalAdjustmentId> id) {
    const std::optional<LocalAdjustmentId> reconciled = reconciledSelection(state_, id);
    if (reconciled == selection_) {
        // Nothing to redraw: a stroke's every result lands here with the selection it had.
        return;
    }
    selection_ = reconciled;
    scheduleCoverage();
    refreshBrushFull();
    update();
}

void MaskOverlay::setTool(MaskTool tool) {
    tool_ = tool;
    scheduleCoverage();
    refreshBrushFull();
    update();
}

void MaskOverlay::setBrushSettings(const BrushSettings& settings) {
    // A stroke under way keeps what it was captured with; the next one has these.
    const std::optional<QRectF> oldRing =
        hover_ ? std::optional<QRectF>(ringRect(*hover_)) : std::nullopt;
    brush_ = settings;
    refreshBrushFull();
    if (stroking()) {
        // The ring alone changes under a stroke (old and new extent); the dabs and the tint stay.
        if (hover_) {
            update((*oldRing | ringRect(*hover_)).toAlignedRect());
        }
        return;
    }
    update();
}

double MaskOverlay::ringRadius() const {
    const std::optional<MaskViewMapping> map = mapping();
    return map ? static_cast<double>(brush_.radius) * map->scale() : 0.0;
}

QRectF MaskOverlay::ringRect(QPointF centre) const {
    const double half = std::max(ringRadius(), crossLength / 2.0) + ringMargin;
    return {centre.x() - half, centre.y() - half, 2.0 * half, 2.0 * half};
}

void MaskOverlay::refreshBrushFull() {
    if (stroking()) {
        return;
    }
    BrushRefusal refusal = BrushRefusal::None;
    if (painting()) {
        const LocalAdjustment* selected = selectedMask();
        if (selected != nullptr && std::holds_alternative<BrushMask>(selected->shape)) {
            refusal = refusalOf(state_, selected->id, probeStroke(brush_, SensorPoint{0.5F, 0.5F}));
        } else if (!canAddLocalAdjustment(state_)) {
            refusal = BrushRefusal::MasksFull;
        }
    }
    brushFull_ = refusal != BrushRefusal::None;
    updateCursor(mapFromGlobal(QCursor::pos()));
}

void MaskOverlay::trackHover(std::optional<QPointF> at) {
    const std::optional<QPointF> was = hover_;
    hover_ = at;
    if (!painting()) {
        return;
    }
    QRectF dirty;
    if (was) {
        dirty |= ringRect(*was);
    }
    if (at) {
        dirty |= ringRect(*at);
    }
    update(dirty.toAlignedRect());
}

void MaskOverlay::refresh(bool consume) {
    if (!stroking()) {
        update();
        return;
    }
    // Only the ring and the dabs added since the last repaint changed.
    QRectF dirty = strokeDirt_;
    if (hover_) {
        dirty |= ringRect(*hover_);
    }
    if (consume) {
        strokeDirt_ = QRectF();
    }
    update(dirty.toAlignedRect());
}

void MaskOverlay::setOverlayShown(bool shown) {
    tint_ = shown;
    scheduleCoverage();
    update();
}

std::optional<MaskViewMapping> MaskOverlay::mapping() const {
    if (!frame_) {
        return std::nullopt;
    }
    return MaskViewMapping(*frame_, view_->transform(), view_->devicePixelRatioF(), lens_);
}

const LocalAdjustment* MaskOverlay::selectedMask() const {
    return selection_ ? findLocalAdjustment(state_, *selection_) : nullptr;
}

std::optional<MaskOverlay::CoverageInputs> MaskOverlay::coverageInputs() const {
    const LocalAdjustment* mask = selectedMask();
    if (!tintWanted() || mask == nullptr || !frame_ || size().isEmpty()) {
        return std::nullopt;
    }
    const ViewTransform t = view_->transform();
    CoverageInputs inputs;
    inputs.shape = mask->shape;
    inputs.invert = mask->invert;
    inputs.geometry = state_.settings.geometry;
    inputs.source = source_;
    inputs.lens = lens_ != nullptr ? *lens_ : LensCorrection{};
    inputs.widget = size();
    inputs.zoom = t.zoom();
    inputs.centre = t.centre();
    inputs.frame = t.frame();
    inputs.ratio = view_->devicePixelRatioF();
    inputs.reduction = reduced_ ? 2 : 1;
    return inputs;
}

bool MaskOverlay::tintWanted() const {
    if (tint_) {
        return true;
    }
    // A brush that changes nothing shows nothing in the render, so the tint is all there is.
    const LocalAdjustment* mask = selectedMask();
    if (!painting() || mask == nullptr || !std::holds_alternative<BrushMask>(mask->shape)) {
        return false;
    }
    return !carriesEffect(*mask);
}

void MaskOverlay::scheduleCoverage() {
    // Even when no tint is wanted any more: the remake clears the one that was drawn.
    if (!coverageTimer_.isActive()) {
        coverageTimer_.start();
    }
}

void MaskOverlay::updateCoverage() {
    coverageTimer_.stop();
    if (gesture_ == Gesture::None) {
        reduced_ = false;
    }
    const auto inputs = coverageInputs();
    if (!inputs) {
        coverageImage_ = {};
        coveredFor_.reset();
        return;
    }
    if (coveredFor_ == inputs && !coverageImage_.isNull()) {
        return;
    }
    const detail::TimingSpan timing("mask.coverage");
    QElapsedTimer clock;
    clock.start();
    try {
        // The part of the developed frame the widget shows, which may reach past the picture.
        const ViewTransform t = view_->transform();
        const double ratio = view_->devicePixelRatioF();
        const QPointF topLeft = t.frameFromView({0.0, 0.0});
        const QPointF bottomRight = t.frameFromView(QPointF(width() * ratio, height() * ratio));
        const DevelopedRegion region{topLeft.x(), topLeft.y(), bottomRight.x() - topLeft.x(),
                                     bottomRight.y() - topLeft.y()};
        double columns = std::max(1.0, std::round(width() * coverageDensity / inputs->reduction));
        double rows = std::max(1.0, std::round(height() * coverageDensity / inputs->reduction));
        if (columns * rows > coverageCells) {
            const double shrink = std::sqrt(coverageCells / (columns * rows));
            columns = std::max(1.0, std::floor(columns * shrink));
            rows = std::max(1.0, std::floor(rows * shrink));
        }
        const ImageSize grid{static_cast<std::uint32_t>(columns), static_cast<std::uint32_t>(rows)};
        const LocalAdjustment& mask = *selectedMask();
        const MaskCoverage covered =
            maskCoverage(mask, *frame_, region, grid, lens_ != nullptr ? *lens_ : LensCorrection{});
        QImage image(static_cast<int>(grid.width), static_cast<int>(grid.height),
                     QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < image.height(); ++y) {
            auto* line = reinterpret_cast<std::uint32_t*>(image.scanLine(y));
            const std::uint8_t* weights =
                covered.weights.data() + static_cast<std::size_t>(y) * grid.width;
            for (int x = 0; x < image.width(); ++x) {
                // Red, premultiplied: the alpha is the tint's, and so is the red.
                const auto alpha = static_cast<std::uint32_t>(
                    std::lround(static_cast<double>(weights[x]) * tintOpacity));
                line[x] = (alpha << 24) | (alpha << 16);
            }
        }
        if (stroking() && image.size() != coverageImage_.size()) {
            // The grid changed: every cell moved, so all of it is repainted.
            strokeDirt_ = QRectF(rect());
        }
        coverageImage_ = std::move(image);
        coveredFor_ = inputs;
    } catch (const std::exception&) {
        coverageImage_ = {};
        coveredFor_.reset();
    }
    coverageMilliseconds_ = static_cast<double>(clock.nsecsElapsed()) / 1e6;
    // Latched for the rest of the gesture, so that the grid does not change size within a drag.
    if (gesture_ != Gesture::None && coverageMilliseconds_ > slowCoverageMilliseconds) {
        reduced_ = true;
    }
    refresh(true);
}

void MaskOverlay::abandonDrag() {
    const bool stroked = gesture_ == Gesture::Stroke;
    gesture_ = Gesture::None;
    stroke_.reset();
    strokeTimer_.stop();
    scheduleCoverage();
    creating_ = MaskTool::None;
    if (stroked) {
        update();
    }
    refreshBrushFull();
}

bool MaskOverlay::cancelGesture() {
    if (gesture_ == Gesture::None) {
        return false;
    }
    const bool edit = gesture_ != Gesture::Pan;
    const bool stroked = gesture_ == Gesture::Stroke;
    gesture_ = Gesture::None;
    stroke_.reset();
    strokeTimer_.stop();
    scheduleCoverage();
    creating_ = MaskTool::None;
    if (edit) {
        emit editCancelled();
    }
    if (stroked) {
        update();
    }
    refreshBrushFull();
    return true;
}

void MaskOverlay::escape() {
    if (cancelGesture()) {
        return;
    }
    if (painting()) {
        const LocalAdjustment* selected = selectedMask();
        if (selected != nullptr && std::holds_alternative<BrushMask>(selected->shape)) {
            // Brush stays armed: the next stroke starts a new brush.
            emit maskSelected(std::nullopt);
            return;
        }
    }
    if (tool_ != MaskTool::None) {
        tool_ = MaskTool::None;
        refreshBrushFull();
        update();
        emit toolChanged(MaskTool::None);
        return;
    }
    emit leaveRequested();
}

void MaskOverlay::toggleOverlay() {
    setOverlayShown(!tint_);
    emit overlayToggled(tint_);
}

void MaskOverlay::deleteSelected() {
    if (!selectedMask() || gesture_ != Gesture::None) {
        return;
    }
    DevelopState next;
    try {
        next = withLocalAdjustmentRemoved(state_, *selection_);
    } catch (const std::exception&) {
        return;
    }
    emit editStarted();
    emit stateEdited(next);
    emit editFinished();
}

bool MaskOverlay::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // Claimed while the mode is on, as the crop overlay claims its keys, so that no
        // window shortcut or focused button takes them first.
        auto* key = static_cast<QKeyEvent*>(event);
        const auto modifiers = key->modifiers() & ~Qt::KeypadModifier;
        if (modifiers == Qt::NoModifier && claimedKey(key->key())) {
            event->accept();
            return true;
        }
        break;
    }
    case QEvent::WindowDeactivate:
    case QEvent::Hide:
        // The pointer may never report its release.
        cancelGesture();
        spaceHeld_ = false;
        altHeld_ = false;
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void MaskOverlay::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    spaceHeld_ = false;
    cancelGesture();
}

void MaskOverlay::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    // Alt may have gone down while another widget had the keys.
    altHeld_ = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::AltModifier);
    refreshBrushFull();
    trackHover(event->position());
}

void MaskOverlay::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    trackHover(std::nullopt);
}

void MaskOverlay::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    scheduleCoverage();
}

void MaskOverlay::wheelEvent(QWheelEvent* event) {
    // The view beneath zooms about the cursor; mid-gesture the mapping must hold still. Handed
    // over rather than ignored, so that it does not depend on how the event was sent.
    if (gesture_ == Gesture::None) {
        QWheelEvent forwarded(event->position() + QPointF(pos()), event->globalPosition(),
                              event->pixelDelta(), event->angleDelta(), event->buttons(),
                              event->modifiers(), event->phase(), event->inverted(),
                              event->source());
        QCoreApplication::sendEvent(view_, &forwarded);
    }
    event->accept();
}

void MaskOverlay::mousePressEvent(QMouseEvent* event) {
    const std::optional<MaskViewMapping> map = mapping();
    // A middle drag, Alt with a left drag and Space with a left drag pan: the view beneath's.
    // While the tool paints, Alt erases rather than pans.
    if (!map || event->button() != Qt::LeftButton || spaceHeld_ ||
        ((event->modifiers() & Qt::AltModifier) && !painting())) {
        if (painting() && gesture_ == Gesture::None) {
            // The view pans now and the overlay hears no more moves: the ring would stay behind.
            trackHover(std::nullopt);
        }
        event->ignore();
        return;
    }
    event->accept();
    if (gesture_ != Gesture::None) {
        return;
    }
    const QPointF at = event->position();
    press_ = last_ = at;
    moved_ = false;
    lastReported_.reset();
    baseline_ = state_;
    if (painting()) {
        trackHover(at);
        startStroke(at, event->modifiers() & Qt::AltModifier, *map);
        return;
    }
    if (tool_ != MaskTool::None) {
        if (!canAddLocalAdjustment(state_)) {
            return;
        }
        creating_ = tool_;
        announced_ = false;
        gesture_ = Gesture::Create;
        emit editStarted();
        return;
    }
    const LocalAdjustment* selected = selectedMask();
    // The dots first, then the other masks' pins, then the band lines: a band line spans the
    // whole view and must not hide a pin.
    MaskHandle handle = MaskHandle::None;
    if (selected != nullptr) {
        handle = handleAt(selected->shape, *map, at, maskHandleReach, false);
    }
    if (handle == MaskHandle::None) {
        if (const auto pin = pinAt(state_, *map, at, maskHandleReach, selection_)) {
            emit maskSelected(pin);
            return;
        }
        if (selected != nullptr) {
            handle = handleAt(selected->shape, *map, at);
        }
    }
    if (handle != MaskHandle::None) {
        handle_ = handle;
        shapeAtPress_ = selected->shape;
        draggedMask_ = selected->id;
        gesture_ = Gesture::Handle;
        emit editStarted();
        return;
    }
    gesture_ = Gesture::Pan;
    setCursor(Qt::ClosedHandCursor);
}

void MaskOverlay::startStroke(QPointF at, bool alt, const MaskViewMapping& map) {
    altHeld_ = alt;
    std::optional<LocalAdjustmentId> target;
    if (const LocalAdjustment* selected = selectedMask();
        selected != nullptr && std::holds_alternative<BrushMask>(selected->shape)) {
        target = selected->id;
    }
    BrushRefusal refused = BrushRefusal::None;
    std::optional<LiveStroke> stroke =
        LiveStroke::begin(baseline_, target, brush_, erasing(), at, map, refused);
    if (!stroke) {
        brushFull_ = true;
        updateCursor(at);
        const QString name = target ? maskDisplayName(state_, *target) : QString();
        emit brushRefused(refused, describeRefusal(refused, name));
        return;
    }
    const LocalAdjustmentId id = stroke->mask();
    stroke_ = std::move(stroke);
    gesture_ = Gesture::Stroke;
    refusalShown_ = false;
    strokeRadius_ = brush_.radius;
    strokeDirt_ = QRectF();
    lastKept_ = at;
    emit editStarted();
    // The press's own result, at once: the first dab shows without waiting for a turn.
    reportStroke();
    if (!target && stroking()) {
        // The mask exists from the first result on, as a created Linear does.
        emit maskSelected(id);
    }
}

void MaskOverlay::growStrokeDirt(QPointF at) {
    if (!tintWanted()) {
        return;
    }
    // The tint changes within a radius of the segment, and a grid cell beyond it.
    // The cell is the tint image's own, which the slow-coverage fallback may have coarsened.
    const double cell = coverageImage_.isNull()
                            ? 1.0 / coverageDensity
                            : static_cast<double>(width()) / coverageImage_.width();
    const std::optional<MaskViewMapping> map = mapping();
    const double radius = map ? static_cast<double>(strokeRadius_) * map->scale() : 0.0;
    const double reach = radius + 2.0 * cell;
    QRectF dirt = QRectF(lastKept_, at).normalized();
    dirt.adjust(-reach, -reach, reach, reach);
    strokeDirt_ |= dirt;
}

void MaskOverlay::reportStroke() {
    strokeTimer_.stop();
    if (!stroke_ || !stroke_->pendingReport()) {
        return;
    }
    // A copy: the receiver may abandon the gesture, which drops the stroke.
    const DevelopState state = stroke_->state();
    stroke_->reported();
    emit stateEdited(state);
}

void MaskOverlay::extendStroke(QPointF at, bool release) {
    const std::optional<MaskViewMapping> map = mapping();
    if (!map) {
        cancelGesture();
        return;
    }
    if (stroke_->atPointCap() && stroke_->pendingReport()) {
        // The stroke the split finishes is the one the cache has seen.
        reportStroke();
        if (!stroke_) {
            return;
        }
    }
    if (stroke_->extend(at, *map, release)) {
        growStrokeDirt(at);
        lastKept_ = at;
        if (!release && !strokeTimer_.isActive()) {
            strokeTimer_.start();
        }
    }
    if (stroke_->refusal() != BrushRefusal::None && !refusalShown_) {
        refusalShown_ = true;
        brushFull_ = true;
        setCursor(Qt::ForbiddenCursor);
        emit brushRefused(stroke_->refusal(),
                          describeRefusal(stroke_->refusal(),
                                          maskDisplayName(stroke_->state(), stroke_->mask()),
                                          true));
    }
}

void MaskOverlay::finishStroke(QPointF at) {
    extendStroke(at, true);
    if (!stroke_) {
        return; // Lost while working out the last position.
    }
    reportStroke();
    if (!stroke_) {
        return;
    }
    stroke_.reset();
    // Still a stroke while the window handles the end: its last request is paced as the others.
    finishing_ = true;
    gesture_ = Gesture::None;
    emit editFinished();
    finishing_ = false;
    refreshBrushFull();
    scheduleCoverage();
    update();
}

void MaskOverlay::continueGesture(QPointF pointer, bool constrain) {
    const std::optional<MaskViewMapping> map = mapping();
    if (!map) {
        cancelGesture();
        return;
    }
    DevelopState next;
    try {
        if (gesture_ == Gesture::Create) {
            next = withLocalAdjustmentAdded(
                baseline_, createdShape(creating_, press_, pointer, *map, QSizeF(size())));
        } else {
            next = withLocalShape(
                baseline_, draggedMask_,
                draggedShape(shapeAtPress_, handle_, press_, pointer, *map, constrain));
        }
    } catch (const std::exception&) {
        // The clamps of MaskEditing should make this impossible; a lost gesture is the safe end.
        cancelGesture();
        return;
    }
    lastReported_ = pointer;
    emit stateEdited(next);
    if (gesture_ == Gesture::Create && !announced_) {
        // The mask exists from the first result on: it shows its handles while it is drawn.
        announced_ = true;
        emit maskSelected(baseline_.nextLocalAdjustmentId);
    }
}

void MaskOverlay::mouseMoveEvent(QMouseEvent* event) {
    const QPointF at = event->position();
    altHeld_ = event->modifiers() & Qt::AltModifier;
    if (painting()) {
        trackHover(at);
    }
    if (gesture_ == Gesture::None) {
        updateCursor(at);
        event->ignore();
        return;
    }
    if (!(event->buttons() & Qt::LeftButton)) {
        // The release went elsewhere: the gesture is lost.
        cancelGesture();
        event->accept();
        return;
    }
    event->accept();
    if (std::hypot(at.x() - press_.x(), at.y() - press_.y()) > maskClickDistance) {
        moved_ = true;
    }
    if (gesture_ == Gesture::Pan) {
        view_->panBy(at - last_);
        last_ = at;
        return;
    }
    last_ = at;
    if (gesture_ == Gesture::Stroke) {
        extendStroke(at, false);
        return;
    }
    continueGesture(at, event->modifiers() & Qt::ShiftModifier);
}

void MaskOverlay::finishGesture(QPointF pointer, bool constrain) {
    const bool creation = gesture_ == Gesture::Create;
    // The last move already reported where a drag ended; a press that never moved reports
    // nothing, so that a click on a handle leaves no step. A creation always reports once.
    const bool report = creation ? !lastReported_ || *lastReported_ != pointer
                                 : lastReported_ && *lastReported_ != pointer;
    if (report) {
        continueGesture(pointer, constrain);
        if (gesture_ == Gesture::None) {
            return; // Lost while working out the last shape.
        }
    }
    gesture_ = Gesture::None;
    creating_ = MaskTool::None;
    scheduleCoverage();
    emit editFinished();
    if (creation) {
        tool_ = MaskTool::None;
        emit toolChanged(MaskTool::None);
    }
}

void MaskOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || gesture_ == Gesture::None) {
        event->ignore();
        return;
    }
    event->accept();
    const QPointF at = event->position();
    if (gesture_ == Gesture::Pan) {
        const bool click = !moved_;
        gesture_ = Gesture::None;
        updateCursor(at);
        if (click && selection_) {
            emit maskSelected(std::nullopt);
        }
        return;
    }
    if (gesture_ == Gesture::Stroke) {
        finishStroke(at);
    } else {
        finishGesture(at, event->modifiers() & Qt::ShiftModifier);
    }
    updateCursor(at);
}

void MaskOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Alt && !event->isAutoRepeat()) {
        altHeld_ = true;
        if (hover_) {
            update(ringRect(*hover_).toAlignedRect());
        }
    }
    if (event->modifiers() & ~Qt::KeypadModifier) {
        QWidget::keyPressEvent(event);
        return;
    }
    switch (event->key()) {
    case Qt::Key_Escape:
        escape();
        break;
    case Qt::Key_O:
        toggleOverlay();
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        deleteSelected();
        break;
    case Qt::Key_Space:
        if (!event->isAutoRepeat()) {
            spaceHeld_ = true;
            if (hover_) {
                update(ringRect(*hover_).toAlignedRect());
            }
            updateCursor(mapFromGlobal(QCursor::pos()));
        }
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void MaskOverlay::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Alt && !event->isAutoRepeat()) {
        altHeld_ = false;
        if (hover_) {
            update(ringRect(*hover_).toAlignedRect());
        }
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = false;
        if (hover_) {
            update(ringRect(*hover_).toAlignedRect());
        }
        updateCursor(mapFromGlobal(QCursor::pos()));
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void MaskOverlay::updateCursor(QPointF pointer) {
    if (gesture_ == Gesture::Pan) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (gesture_ != Gesture::None) {
        return;
    }
    if (spaceHeld_) {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (painting()) {
        setCursor(brushFull_ ? Qt::ForbiddenCursor : Qt::BlankCursor);
        return;
    }
    if (tool_ != MaskTool::None) {
        setCursor(Qt::CrossCursor);
        return;
    }
    const std::optional<MaskViewMapping> map = mapping();
    if (map) {
        const LocalAdjustment* selected = selectedMask();
        if (selected != nullptr &&
            handleAt(selected->shape, *map, pointer, maskHandleReach, false) != MaskHandle::None) {
            setCursor(Qt::SizeAllCursor);
            return;
        }
        if (pinAt(state_, *map, pointer, maskHandleReach, selection_)) {
            setCursor(Qt::PointingHandCursor);
            return;
        }
        if (selected != nullptr && handleAt(selected->shape, *map, pointer) != MaskHandle::None) {
            setCursor(Qt::SizeAllCursor);
            return;
        }
    }
    setCursor(Qt::OpenHandCursor);
}

void MaskOverlay::paintHandles(QPainter& painter, const MaskViewMapping& map) const {
    const LocalAdjustment* selected = selectedMask();
    for (const LocalAdjustment& other : state_.localAdjustments) {
        if (selected == nullptr || other.id != selected->id) {
            if (const std::optional<QPointF> pin = pinPosition(other.shape, map)) {
                drawPin(painter, *pin, other.enabled);
            }
        }
    }
    if (selected == nullptr) {
        return;
    }
    if (const auto* linear = std::get_if<LinearMask>(&selected->shape)) {
        const LinearMarks marks = linearMarks(*linear, map, QRectF(rect()));
        strokeLine(painter, QLineF(marks.from, marks.to), 1.0, Qt::DotLine, 150);
        if (marks.middleLine) {
            strokeLine(painter, *marks.middleLine, 1.0, Qt::SolidLine, 150);
        }
        if (marks.fromLine) {
            strokeLine(painter, *marks.fromLine, 1.5, Qt::SolidLine);
        }
        if (marks.toLine) {
            strokeLine(painter, *marks.toLine, 1.5, Qt::DashLine);
        }
    } else if (const auto* radialShape = std::get_if<RadialMask>(&selected->shape)) {
        const RadialMarks marks = radialMarks(*radialShape, map);
        painter.save();
        painter.setTransform(marks.unitToWidget, true);
        strokeUnitCircle(painter, 1.0, Qt::SolidLine);
        if (marks.inner > 0.0) {
            strokeUnitCircle(painter, marks.inner, Qt::DashLine);
        }
        painter.restore();
    }
    const std::vector<HandlePosition> dots = handlePositions(selected->shape, map);
    if (std::holds_alternative<RadialMask>(selected->shape)) {
        // The stem joins the rotation knob to the radius point it stands beyond.
        QPointF plusX;
        QPointF knob;
        for (const HandlePosition& dot : dots) {
            if (dot.handle == MaskHandle::RadiusPlusX) {
                plusX = dot.position;
            } else if (dot.handle == MaskHandle::RadialRotation) {
                knob = dot.position;
            }
        }
        strokeLine(painter, QLineF(plusX, knob), 1.0, Qt::SolidLine);
    }
    // In reverse grab priority, so that the dot a press would take is the one on top.
    for (auto dot = dots.rbegin(); dot != dots.rend(); ++dot) {
        drawDot(painter, dot->position);
    }
}

void MaskOverlay::paintEvent(QPaintEvent* /*event*/) {
    const detail::TimingSpan timing("overlay.paint");
    const std::optional<MaskViewMapping> map = mapping();
    if (!map) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (tintWanted() && !coverageImage_.isNull() && selectedMask() != nullptr) {
        // Clipped to the picture; the grid covers the whole widget.
        const ViewTransform t = map->view();
        const double ratio = map->devicePixelRatio();
        const QRectF picture(t.viewFromFrame({0.0, 0.0}) / ratio,
                             t.viewFromFrame({1.0, 1.0}) / ratio);
        painter.save();
        painter.setClipRect(picture.intersected(QRectF(rect())));
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(QRectF(rect()), coverageImage_);
        painter.restore();
    }
    if (painting()) {
        // A brush has no handles, and a stroke across another mask's pin must not look grabbable.
        paintBrushCursor(painter, *map);
        return;
    }
    paintHandles(painter, *map);
}

void MaskOverlay::paintBrushCursor(QPainter& painter, const MaskViewMapping& map) const {
    if (!hover_ || brushFull_ || spaceHeld_) {
        return;
    }
    const QPointF at = *hover_;
    const double radius = static_cast<double>(brush_.radius) * map.scale();
    if (radius < crossBelowRadius) {
        const double arm = crossLength / 2.0;
        strokeLine(painter, QLineF(at.x() - arm, at.y(), at.x() + arm, at.y()), 1.0, Qt::SolidLine);
        strokeLine(painter, QLineF(at.x(), at.y() - arm, at.x(), at.y() + arm), 1.0, Qt::SolidLine);
    }
    if (radius >= ringBelowRadius) {
        strokeCircle(painter, at, radius, Qt::SolidLine);
        const double core = static_cast<double>(brush_.hardness) * radius;
        if (brush_.hardness > 0.0F && brush_.hardness < 1.0F && core >= 1.0) {
            strokeCircle(painter, at, core, Qt::DashLine);
        }
    }
    if (erasing()) {
        const double arm = eraseBarLength / 2.0;
        strokeLine(painter, QLineF(at.x() - arm, at.y(), at.x() + arm, at.y()), 2.0, Qt::SolidLine);
    }
}

} // namespace arraw::app
