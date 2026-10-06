#pragma once

#include "CropEditing.h"

#include <GeometrySettings.h>

#include <QCursor>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QVariantAnimation>
#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

namespace arraw::app {

/// @brief Composition guide drawn inside the crop frame while it is dragged.
enum class CropGuide { Thirds, Grid, GoldenRatio, Diagonals };

/// @brief What lies under a position of the crop overlay.
struct CropHit {
    /// @brief Kind of place.
    enum class Kind {
        Outside, ///< Outside the frame: a drag rotates.
        Inside,  ///< Inside the frame: a drag moves the image.
        Handle,  ///< On a corner or an edge: a drag resizes.
    };
    Kind kind = Kind::Outside;
    /// Handle under the position, when ::arraw::app::CropHit::Kind::Handle.
    CropHandle handle = CropHandle::TopLeft;
};

/// @brief Turns and flips an image shown for one geometry to show it for another.
///
/// The image shows the photograph turned and flipped as @p from says, and
/// neither straightened nor cropped (what the crop mode renders, ADR 040);
/// the result shows it as @p to says. Only the quarter-turns and the flips
/// count, and the transform is exact.
[[nodiscard]] QImage reorientedImage(const QImage& image, const GeometrySettings& from,
                                     const GeometrySettings& to);

/// @brief The crop mode: the uncropped, straightened photograph with a crop frame on top.
///
/// Shown over the photo view while cropping (ADR 040). It paints the render of
/// the photograph turned and flipped but neither straightened nor cropped,
/// rotated on screen by the straighten angle, so a rotation never waits for a
/// render; the outside of the crop dimmed; the frame with corner brackets and
/// edge bars; and, while dragging, a composition guide. All rules are
/// CropEditing's; this widget maps the mouse and keys to it and reports each
/// change. Until the first render arrives it shows a placeholder, so the mode
/// never opens on an empty frame.
///
/// Mouse: a handle resizes, inside moves the image under the frame, outside
/// rotates about the frame's centre, and Ctrl with a drag (or any drag while
/// straightening) draws a line to straighten along. Keys: Enter or a
/// double-click inside accepts, Esc rejects, O cycles the guide, X swaps
/// portrait and landscape. It holds no edit itself: the owner opens one edit
/// for the whole session (ADR 022) and closes it on finished. Within the
/// session each gesture is a step that undo() and redo() walk through.
class CropOverlay : public QWidget {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(CropOverlay)
public:
    explicit CropOverlay(QWidget* parent = nullptr);

    /// @brief Starts a session over a photograph's geometry, forgetting any image.
    void start(CropEditing editing);

    /// @brief Ends the session, without emitting finished.
    void stop();

    /// @brief Tells whether a session is running.
    [[nodiscard]] bool isActive() const noexcept {
        return editing_.has_value();
    }

    /// @brief Gives the session's model; only while active.
    [[nodiscard]] const CropEditing& editing() const {
        return *editing_;
    }

    /// @brief Shows a render of the photograph turned and flipped, not straightened or cropped.
    /// @param image The render, of the session's geometry as it is now.
    void setImage(const QImage& image);

    /// @brief Shows a render made for an earlier geometry of the session.
    ///
    /// Turned and flipped to the geometry as it is now, so a render that was
    /// on its way when a quarter-turn or a flip happened shows the right way.
    /// @param image The render.
    /// @param renderedFor Geometry it was rendered for.
    void setImage(const QImage& image, const GeometrySettings& renderedFor);

    /// @brief Shows stand-ins for the render until the first one arrives.
    /// @param uncropped The whole photograph in the session's geometry, however rough (a
    /// camera preview); may be null.
    /// @param framed What the crop frame holds, as shown outside the mode (the developed,
    /// straightened, cropped frame); may be null. Dropped at the first change of geometry.
    void setPlaceholder(const QImage& uncropped, const QImage& framed);

    /// @brief Gives the placeholder the whole photograph once it has been read (ADR 043).
    ///
    /// Keeps what the frame holds. Ignored once a render is shown, or for a null image.
    /// @param uncropped The whole photograph in the session's geometry as it is now.
    void fillPlaceholder(const QImage& uncropped);

    /// @brief Gives the image shown beneath the frame: a render, a placeholder or none.
    [[nodiscard]] const QImage& image() const noexcept {
        return image_;
    }

    /// @brief Tells whether anything of the photograph is shown: a render, or a placeholder of
    /// the whole photograph or of what the frame holds.
    [[nodiscard]] bool showsPhotograph() const noexcept {
        return !image_.isNull() || !framed_.isNull();
    }

    /// @brief Tells whether a render, rather than a placeholder, is shown.
    [[nodiscard]] bool hasRender() const noexcept {
        return rendered_;
    }

    /// @brief Gives the size, in device pixels, worth rendering that image at.
    ///
    /// Enough for the scale it is shown at, rounded up to a step of an eighth of an octave
    /// so that small changes ask for nothing; never larger than the photograph.
    [[nodiscard]] QSize renderSize() const;

    /// @brief Applies a command to the model, emitting geometryEdited if it changed anything.
    void edit(const std::function<void(CropEditing&)>& command);

    /// @brief Takes a geometry from elsewhere, such as a slider, without emitting geometryEdited.
    ///
    /// One step of the session's history, unless a step is open (beginStep()).
    /// @throws std::invalid_argument if it is not valid; nothing changes.
    void adopt(const GeometrySettings& geometry);

    /// @brief Opens a step of the session's history that spans several changes, such as a
    /// slider's edit; steps nest.
    void beginStep();

    /// @brief Closes a step opened with beginStep().
    void endStep();

    /// @brief Tells whether undo() would change anything.
    [[nodiscard]] bool canUndo() const noexcept;

    /// @brief Tells whether redo() would change anything.
    [[nodiscard]] bool canRedo() const noexcept;

    /// @brief Steps back through the session's gestures, never past its start.
    ///
    /// Emits geometryEdited for the change. Does nothing during a drag.
    void undo();

    /// @brief Makes the latest undone gesture again.
    void redo();

    /// @brief Gives the guide drawn while dragging.
    [[nodiscard]] CropGuide guide() const noexcept {
        return guide_;
    }

    /// @brief Chooses the guide drawn while dragging.
    void setGuide(CropGuide guide);

    /// @brief Moves on to the next guide, after the last back to the first.
    void cycleGuide();

    /// @brief Tells whether the next drag draws a line to straighten along.
    [[nodiscard]] bool isStraightening() const noexcept {
        return straightening_;
    }

    /// @brief Arms or disarms the straighten tool; a drawn line disarms it.
    void setStraightening(bool straightening);

    /// @brief Tells what lies under a position.
    /// @param position Logical pixels from the widget's top-left corner.
    [[nodiscard]] CropHit hitAt(QPointF position) const;

    /// @brief Maps a point of the upright frame to the widget, as currently shown.
    /// @return Logical pixels from the widget's top-left corner.
    [[nodiscard]] QPointF widgetFromUpright(CropPoint point) const;

    /// @brief Maps a widget position to the upright frame, as currently shown.
    [[nodiscard]] CropPoint uprightFromWidget(QPointF position) const;

    /// @brief Gives the crop frame in the widget, as currently shown.
    [[nodiscard]] QRectF cropRect() const;

    /// @brief Gives the bars drawn for a handle: two arms of a corner bracket, or an edge's bar.
    ///
    /// hitAt() grabs the handle anywhere on them, and some way around them.
    /// @return Rectangles in logical pixels from the widget's top-left corner.
    [[nodiscard]] std::vector<QRectF> handleMarks(CropHandle handle) const;

public slots:
    /// @brief Ends the session keeping the edit.
    void accept();

    /// @brief Ends the session dropping the edit.
    void reject();

    /// @brief Does what Esc does: disarms the straighten tool if it is armed, else rejects.
    void dismiss();

signals:
    /// Emitted when the geometry changed, with the new geometry.
    void geometryEdited(const arraw::GeometrySettings& geometry);
    /// Emitted when the user ends the session; the overlay is stopped already.
    void finished(bool accepted);
    /// Emitted when renderSize() changed, so a new render is worth asking for.
    void renderWanted();
    /// Emitted when the straighten tool is armed or disarmed.
    void straighteningChanged(bool straightening);
    /// Emitted when canUndo() or canRedo() may have changed.
    void historyChanged();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    /// @brief Placement of the upright frame: widget = origin + scale * upright.
    struct Mapping {
        double scale = 1.0;
        QPointF origin;
    };

    /// @brief What a drag in progress does.
    enum class Drag { None, Resize, Move, Rotate, Line };

    /// @brief Gives the placement that fits the whole upright frame in the widget.
    [[nodiscard]] Mapping fitted() const;

    /// @brief Gives the placement in use: held during a move or a rotation, else fitted.
    [[nodiscard]] Mapping mapping() const;

    /// @brief Reports a change of geometry and of the render size worth having.
    void changed();

    /// @brief Makes the cursor suit what lies under a position.
    void updateCursor(QPointF position, Qt::KeyboardModifiers modifiers);

    /// @brief Eases from the placement during a drag to the fitted one.
    void settle(const Mapping& from);

    /// @brief Ends the session, reporting how.
    void finish(bool accepted);

    /// @brief Gives the cursor shown outside the frame, drawn for the screen's pixel ratio.
    [[nodiscard]] const QCursor& rotationCursor();

    std::optional<CropEditing> editing_;
    /// Render or placeholder beneath the frame, turned and flipped as imageGeometry_ says.
    QImage image_;
    /// The image_ in the surface's format, made when first painted; null until then.
    QPixmap pixmap_;
    /// Geometry image_ is turned and flipped for.
    GeometrySettings imageGeometry_;
    /// Whether image_ is a render rather than a placeholder.
    bool rendered_ = false;
    /// Placeholder of the crop frame's contents, for the geometry the session began with.
    QImage framed_;
    /// Geometry framed_ is right for.
    GeometrySettings framedGeometry_;
    /// Cursor outside the frame, and the pixel ratio it was drawn for.
    QCursor rotationCursor_;
    double rotationCursorRatio_ = 0.0;
    CropGuide guide_ = CropGuide::Thirds;
    bool straightening_ = false;

    Drag drag_ = Drag::None;
    CropHandle handle_ = CropHandle::TopLeft;
    /// Handle's position less the pointer's as a resize began, in the upright frame.
    CropPoint grabOffset_;
    QPointF press_;
    QPointF pointer_;
    /// Placement as the drag began.
    Mapping pressMapping_;
    /// Crop frame centre in the widget as a rotation began, which it keeps.
    QPointF pivot_;
    /// Angle on screen as a rotation began, and of the pointer about the pivot.
    double startAngle_ = 0.0;
    double startPointerAngle_ = 0.0;
    /// Geometry last reported.
    GeometrySettings reported_;
    QSize reportedRenderSize_;

    /// Placement eased away from after a drag, and how far, from 0 to 1.
    std::optional<Mapping> settleFrom_;
    double settleProgress_ = 1.0;
    QVariantAnimation settleAnimation_;
};

} // namespace arraw::app
