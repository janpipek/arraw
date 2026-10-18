#pragma once

#include "BrushPainting.h"
#include "MaskEditing.h"

#include <CropGeometry.h>
#include <DevelopState.h>
#include <GeometrySettings.h>
#include <LensCorrection.h>
#include <LocalAdjustments.h>

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <optional>

class QEnterEvent;
class QEvent;
class QFocusEvent;
class QPainter;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;

namespace arraw::app {

class PhotoView;

/// @brief The mask mode: the handles and pins of the masks, and the tint of the selected one,
/// over the photograph (ADR 044, section 10).
///
/// A transparent child of PhotoView that covers it and is shown in the mask mode only. It paints
/// the selected mask's handles, a pin at the centre of every other mask, and, when asked, a red
/// tint where the selected mask applies; and it maps the mouse and keys to edits. The maths of
/// the mapping, the hits and the drags is MaskEditing's; this widget holds no edit itself. It
/// reports in the protocol of the develop panel (ADR 022): editStarted, a stateEdited for every
/// change, then editFinished, or editCancelled when the gesture is lost.
///
/// Mouse: with a tool armed a drag creates a mask (Linear from the press to the release, Radial
/// from the centre to the radius) and a click creates the default; otherwise a handle of the
/// selected mask is dragged, a pin selects its mask, and a drag on empty canvas pans the view,
/// a click there clearing the selection. A middle drag, Alt with a left drag, and a left drag
/// while Space is held pan; the wheel zooms: all of those are left to the photo view beneath.
///
/// With Brush armed the tool paints: a press starts a stroke on the selected brush, or on a new
/// one, every move extends it and the release ends it, each as one gesture in the same protocol;
/// the moves of one turn of the event loop make one stateEdited. No handles or pins are drawn
/// then; the cursor is a ring of the brush's size instead, with a dashed ring at its hardness
/// and a bar while the stroke would erase (the Erase setting, inverted by Alt held).
///
/// Keys: Esc cancels a gesture, else (with Brush armed and a brush selected) deselects it, else
/// disarms the tool, else asks to leave the mode; O shows or hides the tint; Delete and Backspace
/// delete the selected mask; Space held turns a drag into a pan.
///
/// A gesture is cancelled when the pointer is lost. Qt Widgets has no event for a lost implicit
/// grab, so the window being deactivated, the overlay hidden or losing the focus, and a move
/// without the button held, all count as lost.
class MaskOverlay : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MaskOverlay)
public:
    /// @brief Makes the overlay over a photo view; hidden until the view shows it.
    /// @param view The view it covers and whose transform it maps through.
    explicit MaskOverlay(PhotoView* view);

    /// @brief Shows the state of the photograph and where its corrected frame lies.
    ///
    /// Not an edit: a gesture in progress carries on from the state it began with.
    /// @param state State to draw the masks of; also the base of the next edit.
    /// @param source Shape of the full photograph; nothing without one, when the overlay
    /// draws and takes nothing.
    void setScene(const DevelopState& state, const std::optional<SourceShape>& source);

    /// @brief Shows the lens corrections of the render the overlay lies over.
    ///
    /// A brush stroke is stored in the sensor frame and shown in the corrected one: the tint is
    /// drawn through the lens, and a stroke painted from now on is stored where the lens maps the
    /// pointer from (ADR 009, ADR 050, section 7). Not an edit.
    /// @param lens Corrections as resolved for the render; an empty one for none.
    void setLens(const LensCorrection& lens);

    /// @brief Shows which mask has handles, without emitting any signal.
    void setSelection(std::optional<LocalAdjustmentId> id);

    /// @brief Gives the mask that has handles.
    [[nodiscard]] std::optional<LocalAdjustmentId> selection() const noexcept {
        return selection_;
    }

    /// @brief Arms or disarms a creation tool, without emitting any signal.
    void setTool(MaskTool tool);

    /// @brief Gives the settings the next stroke is made with.
    void setBrushSettings(const BrushSettings& settings);

    /// @brief Tells whether the tool paints: the brush is armed.
    [[nodiscard]] bool painting() const noexcept {
        return tool_ == MaskTool::Brush;
    }

    /// @brief Tells whether a stroke is under way.
    [[nodiscard]] bool stroking() const noexcept {
        return gesture_ == Gesture::Stroke || finishing_;
    }

    /// @brief Gives the radius of the brush's ring, in logical pixels; zero without a photograph.
    [[nodiscard]] double ringRadius() const;

    /// @brief Gives the armed tool.
    [[nodiscard]] MaskTool tool() const noexcept {
        return tool_;
    }

    /// @brief Shows or hides the tint of the selected mask, without emitting any signal.
    void setOverlayShown(bool shown);

    /// @brief Tells whether the tint is on.
    [[nodiscard]] bool overlayShown() const noexcept {
        return tint_;
    }

    /// @brief Gives the tint as last made, in the widget's logical size; null before the first.
    [[nodiscard]] const QImage& coverage() const noexcept {
        return coverageImage_;
    }

    /// @brief Tells whether a gesture is under way.
    [[nodiscard]] bool isDragging() const noexcept {
        return gesture_ != Gesture::None;
    }

    /// @brief Stops tracking a gesture without reporting anything.
    ///
    /// For when the owner is about to end the edit itself, such as an undo during a drag.
    void abandonDrag();

    /// @brief Cancels a gesture under way, emitting editCancelled.
    /// @return Whether there was one.
    bool cancelGesture();

    /// @brief Does what Esc does: cancels the gesture, else disarms the tool, else asks to leave.
    void escape();

    /// @brief Shows or hides the tint, as the O key does, emitting overlayToggled.
    void toggleOverlay();

    /// @brief Deletes the selected mask as one edit, if there is one.
    void deleteSelected();

    /// @brief Gives the pointer's mapping from the corrected frame to the widget.
    /// @return Nothing without a photograph.
    [[nodiscard]] std::optional<MaskViewMapping> mapping() const;

    /// @brief Makes the tint now, if it is wanted and out of date, instead of on the next turn
    /// of the event loop.
    void updateCoverage();

signals:
    /// @brief Announces that an edit begins.
    void editStarted();

    /// @brief Announces the state an edit has reached.
    /// @param state Base state with the gesture's result.
    void stateEdited(const arraw::DevelopState& state);

    /// @brief Announces that the edit is over.
    void editFinished();

    /// @brief Announces that the gesture was lost and the edit is to be dropped.
    void editCancelled();

    /// @brief Announces a mask the user chose by a click, or none by a click on empty canvas.
    void maskSelected(std::optional<arraw::LocalAdjustmentId> id);

    /// @brief Announces that the tool is disarmed, as after a creation or by Esc.
    void toolChanged(arraw::app::MaskTool tool);

    /// @brief Announces that a press or a stroke was refused by a cap or budget of the brush.
    ///
    /// Emitted once per press, or once per stroke when it is stopped.
    /// @param refusal Reason.
    /// @param message The words for the status line.
    void brushRefused(arraw::app::BrushRefusal refusal, const QString& message);

    /// @brief Announces that the O key toggled the tint.
    void overlayToggled(bool shown);

    /// @brief Asks the window to leave the mask mode (Esc with nothing to cancel or disarm).
    void leaveRequested();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    /// What a gesture is.
    enum class Gesture { None, Handle, Create, Pan, Stroke };

    /// What the tint was made from, to tell whether it is out of date.
    struct CoverageInputs {
        Mask shape;
        bool invert = false;
        GeometrySettings geometry;
        std::optional<SourceShape> source;
        LensCorrection lens;
        QSize widget;
        double zoom = 0.0;
        QPointF centre;
        QSizeF frame;
        double ratio = 1.0;
        int reduction = 1;

        friend bool operator==(const CoverageInputs&, const CoverageInputs&) = default;
    };

    /// @brief Gives the selected mask, or null.
    [[nodiscard]] const LocalAdjustment* selectedMask() const;

    /// @brief Gives what the tint would be made from now, or nothing when it is not wanted.
    [[nodiscard]] std::optional<CoverageInputs> coverageInputs() const;

    /// @brief Tells whether the tint is drawn: the O key's, or the brush has no other effect.
    [[nodiscard]] bool tintWanted() const;

    /// @brief Asks for the tint to be remade on the next turn of the event loop.
    void scheduleCoverage();

    /// @brief Reports the gesture's result for a pointer position.
    void continueGesture(QPointF pointer, bool constrain);

    /// @brief Ends a creation or a handle drag, reporting it.
    void finishGesture(QPointF pointer, bool constrain);

    /// @brief Starts a stroke at a press, reporting it, or says why it cannot start.
    void startStroke(QPointF at, bool alt, const MaskViewMapping& map);

    /// @brief Adds a pointer position to the stroke.
    void extendStroke(QPointF at, bool release);

    /// @brief Ends the stroke at the release, reporting what is left of it.
    void finishStroke(QPointF at);

    /// @brief Reports the stroke's state if it changed since the last report.
    void reportStroke();

    /// @brief Remembers the part of the widget a kept point changes, for the next repaint.
    void growStrokeDirt(QPointF at);

    /// @brief Repaints what changed: the whole widget, or while stroking the ring and the new
    /// dabs only.
    /// @param consume Whether the dabs' region is spent by it.
    void refresh(bool consume);

    /// @brief Moves the ring to a pointer position, repainting where it was and where it is.
    void trackHover(std::optional<QPointF> at);

    /// @brief Gives the part of the widget the ring at a position covers.
    [[nodiscard]] QRectF ringRect(QPointF centre) const;

    /// @brief Finds out whether the brush could take one more stroke, for the cursor.
    void refreshBrushFull();

    /// @brief Tells whether the next stroke would erase: the setting, inverted by Alt.
    [[nodiscard]] bool erasing() const noexcept {
        return brush_.erase != altHeld_;
    }

    /// @brief Paints the brush's ring at the pointer.
    void paintBrushCursor(QPainter& painter, const MaskViewMapping& mapping) const;

    /// @brief Makes the cursor suit what is under the pointer.
    void updateCursor(QPointF pointer);

    /// @brief Paints a mask's handles.
    void paintHandles(QPainter& painter, const MaskViewMapping& mapping) const;

    /// Photo view this covers.
    PhotoView* view_;
    DevelopState state_;
    std::optional<SourceShape> source_;
    /// Corrected frame in the developed one, for the state's geometry; nothing without a source.
    std::optional<DevelopedFrameMap> frame_;
    /// Lens corrections of the render shown; null for none.
    std::shared_ptr<const LensCorrection> lens_;
    std::optional<LocalAdjustmentId> selection_;
    MaskTool tool_ = MaskTool::None;
    bool tint_ = false;
    /// Whether the window is handling the end of a stroke, which still counts as one.
    bool finishing_ = false;
    bool spaceHeld_ = false;
    /// Whether Alt is held, which inverts the brush's Erase setting.
    bool altHeld_ = false;

    /// Settings the next stroke is made with.
    BrushSettings brush_;
    /// Radius the stroke under way started with, in the brush's units.
    float strokeRadius_ = 0.0F;
    /// Stroke under way; nothing otherwise.
    std::optional<LiveStroke> stroke_;
    /// Reports the stroke once per turn of the event loop.
    QTimer strokeTimer_;
    /// Pointer over the overlay while painting; nothing when it is elsewhere.
    std::optional<QPointF> hover_;
    /// Whether the brush cannot take one more stroke: the cursor says so.
    bool brushFull_ = false;
    /// Whether the stroke's refusal was announced.
    bool refusalShown_ = false;
    /// Where the last point the stroke kept lies in the widget.
    QPointF lastKept_;
    /// Part of the widget the tint changed in since it was last repainted, while stroking.
    QRectF strokeDirt_;

    Gesture gesture_ = Gesture::None;
    /// State when the gesture began: what each result is built from.
    DevelopState baseline_;
    MaskHandle handle_ = MaskHandle::None;
    Mask shapeAtPress_;
    /// Mask the handle drag works on, kept from the press.
    LocalAdjustmentId draggedMask_;
    QPointF press_;
    QPointF last_;
    /// Pointer position the last result was worked out for.
    std::optional<QPointF> lastReported_;
    bool moved_ = false;
    MaskTool creating_ = MaskTool::None;
    /// Whether the mask being created has been announced as selected.
    bool announced_ = false;

    QTimer coverageTimer_;
    QImage coverageImage_;
    std::optional<CoverageInputs> coveredFor_;
    /// Duration of the last tint, in milliseconds, to thin the grid while dragging.
    double coverageMilliseconds_ = 0.0;
    /// Whether the grid is thinned, latched for the rest of a gesture once the tint was slow.
    bool reduced_ = false;
};

} // namespace arraw::app
