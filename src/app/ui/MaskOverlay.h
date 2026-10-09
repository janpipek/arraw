#pragma once

#include "MaskEditing.h"

#include <CropGeometry.h>
#include <DevelopState.h>
#include <GeometrySettings.h>
#include <LocalAdjustments.h>

#include <QImage>
#include <QPointF>
#include <QTimer>
#include <QWidget>

#include <optional>

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
/// Keys: Esc cancels a gesture, else disarms the tool, else asks to leave the mode; O shows or
/// hides the tint; Delete and Backspace delete the selected mask; Space held turns a drag into a
/// pan.
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

    /// @brief Shows which mask has handles, without emitting any signal.
    void setSelection(std::optional<LocalAdjustmentId> id);

    /// @brief Gives the mask that has handles.
    [[nodiscard]] std::optional<LocalAdjustmentId> selection() const noexcept {
        return selection_;
    }

    /// @brief Arms or disarms a creation tool, without emitting any signal.
    void setTool(MaskTool tool);

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

private:
    /// What a gesture is.
    enum class Gesture { None, Handle, Create, Pan };

    /// What the tint was made from, to tell whether it is out of date.
    struct CoverageInputs {
        Mask shape;
        bool invert = false;
        GeometrySettings geometry;
        std::optional<SourceShape> source;
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

    /// @brief Asks for the tint to be remade on the next turn of the event loop.
    void scheduleCoverage();

    /// @brief Reports the gesture's result for a pointer position.
    void continueGesture(QPointF pointer, bool constrain);

    /// @brief Ends a creation or a handle drag, reporting it.
    void finishGesture(QPointF pointer, bool constrain);

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
    std::optional<LocalAdjustmentId> selection_;
    MaskTool tool_ = MaskTool::None;
    bool tint_ = false;
    bool spaceHeld_ = false;

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
