#pragma once

#include "ViewTransform.h"

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;

namespace arraw::app {

class CropOverlay;
class MaskOverlay;

/// @brief Widget that shows a part of the developed photograph, zoomed and panned.
///
/// Holds the view (a zoom and the frame point at its middle, see ViewTransform)
/// and the newest render, and paints that render where the current view puts
/// it. Zooming and panning therefore show at once, stretched or shifted, until
/// the window has a render of the new view; the owner asks for it on
/// viewChanged. A reduced whole-frame image fills areas outside that render.
/// It renders nothing itself.
///
/// The wheel zooms about the cursor. A left drag pans, unless picking, when a
/// left click picks; a middle drag and Alt with a left drag pan either way.
///
/// In the crop mode a CropOverlay covers it and takes the mouse, the wheel and
/// the keys (ADR 040); the view beneath keeps its zoom for when the mode ends. In the mask mode
/// a MaskOverlay covers it instead, leaving the wheel and the pans to the view beneath; the two
/// overlays are never shown together.
class PhotoView : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PhotoView)
public:
    explicit PhotoView(QWidget* parent = nullptr);

    /// @brief Gives the view as it is now, for sizing a render.
    [[nodiscard]] ViewTransform transform() const;

    /// @brief Gives the zoom, in device pixels per photograph pixel.
    [[nodiscard]] double zoom() const;

    /// @brief Tells whether the whole frame is fitted, and kept so on resizing.
    [[nodiscard]] bool isFit() const noexcept {
        return fit_;
    }

    /// @brief Gives the view's size in device pixels.
    [[nodiscard]] QSize devicePixels() const;

    /// @brief Sets the size of the developed frame at full resolution.
    ///
    /// Changes when an edit changes the crop. Keeps the view, re-fitting if it
    /// is fitting. Emits zoomChanged if the zoom changed, never viewChanged: the
    /// caller is about to render.
    /// @param frame Size in photograph pixels; empty if there is no photograph.
    void setFrameSize(QSize frame);

    /// @brief Shows a render in place of the one before.
    /// @param image The render.
    /// @param region Part of the frame it shows, in normalised coordinates.
    /// @param background Reduced whole-frame image to fill newly exposed areas.
    void setImage(const QImage& image, const QRectF& region, const QImage& background);

    /// @brief Replaces the whole-frame image beneath the render.
    /// @param background Reduced whole-frame image, refreshed after the render.
    void setBackground(const QImage& background);

    /// @brief Shows a stand-in for the photograph until its first render (ADR 043).
    ///
    /// Painted over the whole frame, shape kept, only while there is no render; the next
    /// setImage() or resetView() drops it. Never what wholeFrameImage() gives.
    /// @param standIn Rough image of the whole frame, such as a thumbnail; null for none.
    void setStandIn(const QImage& standIn);

    /// @brief Gives the best image of the whole developed frame there is: the render if it
    /// shows all of it, else the reduced one beneath; null before the first render.
    [[nodiscard]] QImage wholeFrameImage() const;

    /// @brief Fits the whole frame and keeps it fitted; for a newly opened photograph.
    ///
    /// Does not emit viewChanged: the caller renders.
    void resetView();

    /// @brief Arms or disarms picking: a left click emits picked instead of panning.
    void setPicking(bool picking);

    /// @brief Shows or hides the crop overlay over the view, giving it the focus when shown.
    ///
    /// While shown the overlay is the view's focus proxy, so focus given to the
    /// view goes to it.
    ///
    /// Starting and stopping its session is the caller's, through cropOverlay().
    void setCropMode(bool cropping);

    /// @brief Tells whether the crop overlay is shown.
    [[nodiscard]] bool isCropMode() const;

    /// @brief Shows or hides the mask overlay over the view, giving it the focus when shown.
    ///
    /// Showing it hides the crop overlay, and showing the crop overlay hides it. While shown the
    /// mask overlay is the view's focus proxy.
    void setMaskMode(bool masking);

    /// @brief Tells whether the mask overlay is shown.
    [[nodiscard]] bool isMaskMode() const;

    /// @brief Gives the mask overlay, a child that covers the view.
    [[nodiscard]] MaskOverlay& maskOverlay() const noexcept {
        return *mask_;
    }

    /// @brief Pans the view by a drag the mask overlay saw.
    /// @param delta How far the pointer moved, in logical pixels; the photograph follows it.
    void panBy(QPointF delta);

    /// @brief Gives the crop overlay, a child that covers the view.
    [[nodiscard]] CropOverlay& cropOverlay() const noexcept {
        return *crop_;
    }

public slots:
    /// @brief Fits the whole frame, and keeps it fitted on resizing.
    void zoomToFit();

    /// @brief Zooms to a level about the middle of the view.
    /// @param zoom Device pixels per photograph pixel.
    void zoomTo(double zoom);

    /// @brief Multiplies the zoom, about the middle of the view.
    void zoomBy(double factor);

signals:
    /// Emitted when the user changed what is in view, so a new render is due.
    void viewChanged();
    /// Emitted when the zoom or whether it is fitting changed, for any reason.
    void zoomChanged();
    /// Emitted whenever the view's transform may have changed, for any reason: a zoom, a pan,
    /// the frame, the size or the pixel ratio. Never means a render is due; see viewChanged.
    void transformChanged();
    /// Emitted by a click while picking, with the point in fractions of the frame
    /// (inside [0, 1]).
    void picked(QPointF point);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    /// @brief Adopts a transform as the view, emitting what changed.
    /// @param next The new view.
    /// @param fit Whether it is the fitting one.
    /// @param user Whether the user did it, so that a render is due.
    void adopt(const ViewTransform& next, bool fit, bool user);

    /// @brief Shows one overlay and hides the other, moving the focus with the mode.
    void showOverlay(QWidget* shown, QWidget* hidden);

    /// @brief Makes the cursor suit the mode and the drag.
    void updateCursor();

    /// Crop mode's overlay; hidden outside the mode.
    CropOverlay* crop_ = nullptr;
    /// Mask mode's overlay; hidden outside the mode.
    MaskOverlay* mask_ = nullptr;
    /// Full-resolution frame size; empty without a photograph.
    QSize frame_;
    /// Device pixels per photograph pixel.
    double zoom_ = 1.0;
    /// Frame point at the view's middle, in fractions.
    QPointF centre_{0.5, 0.5};
    bool fit_ = true;
    bool picking_ = false;
    bool dragging_ = false;
    QPointF lastPosition_;

    /// The newest render.
    QImage image_;
    /// Reduced whole-frame fallback.
    QImage background_;
    /// Part of the frame it shows, in fractions of the frame it was rendered for.
    QRectF imageRegion_;
    /// Rough image of the whole frame, shown until the first render; null when none.
    QImage standIn_;
};

} // namespace arraw::app
