#pragma once

#include <CropGeometry.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>

#include <array>
#include <optional>
#include <vector>

namespace arraw::app {

/// @brief One of the eight handles of the crop frame.
enum class CropHandle { TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };

/// @brief Every handle, clockwise from the top-left corner.
inline constexpr std::array<CropHandle, 8> cropHandles{
    CropHandle::TopLeft,     CropHandle::Top,    CropHandle::TopRight,   CropHandle::Right,
    CropHandle::BottomRight, CropHandle::Bottom, CropHandle::BottomLeft, CropHandle::Left};

/// @brief Smallest side of a crop a handle drag leaves, as a fraction of the photograph's shorter
/// side.
inline constexpr double minimumCropFraction = 0.02;

/// @brief Tells whether a handle is a corner, which moves two edges, rather than an edge.
[[nodiscard]] bool isCorner(CropHandle handle) noexcept;

/// @brief Gives the handle's position on a box.
[[nodiscard]] CropPoint handlePosition(const CropBox& box, CropHandle handle) noexcept;

/// @brief Gestures, run of rotations and history of one crop session, over its geometry settings.
///
/// The interaction model of the crop mode (ADR 040), without Qt, as
/// CurveEditing is for the curve editor: it keeps the geometry, the
/// photograph's decoded size and orientation, the gesture under way, the run
/// of rotations and the session's history. The rules that turn a request into
/// the next geometry live in CropGeometry.h; every operation here applies one
/// of them, so an explicit crop stays well formed, agreeing with a locked
/// aspect, and inside valid rotated content (ADR 014's contract).
///
/// Positions are in the uncropped upright frame of the current geometry, in
/// its edge units, which are the photograph's pixels: an isotropic scale of
/// what the screen shows, so a widget converts with one factor and an offset.
///
/// A gesture (a handle drag or an image move) works every step out from the
/// geometry at beginGesture(), so steps do not accumulate rounding or
/// shrinking. Rotation works out from the geometry before the latest run of
/// rotations, so turning back undoes the shrinking a turn caused, whether the
/// run came from a drag, a slider or a typed angle.
///
/// It also keeps the history of a crop session, one step per gesture (a drag,
/// a command, a slider's edit), so that undo can walk back through the
/// session's gestures as Lightroom's crop tool does (ADR 040). The history
/// begins empty: undo never goes past the geometry the editing began with.
class CropEditing {
public:
    /// @brief Starts editing.
    /// @param sourceSize Size of the decoded photograph; not empty.
    /// @param orientation Camera orientation of the decoded photograph.
    /// @param geometry Geometry as it is.
    /// @throws std::invalid_argument if the geometry is not valid for the photograph.
    CropEditing(ImageSize sourceSize, ImageOrientation orientation, GeometrySettings geometry);

    /// @brief Gives the geometry as edited so far.
    [[nodiscard]] const GeometrySettings& geometry() const noexcept {
        return geometry_;
    }

    /// @brief Gives the width of the uncropped upright frame.
    [[nodiscard]] double uprightWidth() const noexcept {
        return frame_.uprightWidth;
    }

    /// @brief Gives the height of the uncropped upright frame.
    [[nodiscard]] double uprightHeight() const noexcept {
        return frame_.uprightHeight;
    }

    /// @brief Gives the crop as the engine resolves it, automatic framing included.
    [[nodiscard]] const CropBox& crop() const noexcept {
        return frame_.crop;
    }

    /// @brief Gives the corners of the valid content, in order around it.
    [[nodiscard]] const std::array<CropPoint, 4>& content() const noexcept {
        return frame_.content;
    }

    /// @brief Gives the size of the photograph turned and flipped but not straightened.
    ///
    /// What a render with no straighten and no crop shows; the crop mode
    /// rotates that render on screen (ADR 040).
    [[nodiscard]] CropBox unstraightened() const noexcept {
        return frame_.unstraightened;
    }

    /// @brief Gives the straighten as it appears on screen (see displayedStraighten()).
    [[nodiscard]] double displayedAngle() const noexcept {
        return displayedStraighten(geometry_);
    }

    /// @brief Gives the width-over-height ratio a locked aspect resolves to, or nothing when free.
    [[nodiscard]] std::optional<double> lockedRatio() const;

    /// @brief Gives the smallest side a handle drag leaves.
    [[nodiscard]] double minimumSide() const noexcept;

    /// @brief Remembers the geometry as a drag begins; resizeTo() and moveImageBy() start from it.
    void beginGesture();

    /// @brief Drags a handle to a position.
    ///
    /// The opposite corner, or the opposite edge, stays where it was; with a
    /// locked aspect an edge's neighbours grow about the middle of that edge.
    /// The crop stops at valid content and at minimumSide(), and never turns
    /// inside out. The result is explicit.
    /// @param handle Handle dragged.
    /// @param pointer Where the pointer is.
    void resizeTo(CropHandle handle, CropPoint pointer);

    /// @brief Moves the image under the crop by a distance since beginGesture().
    ///
    /// The crop moves the other way, sliding along the edges of valid content
    /// rather than leaving it. The result is explicit.
    /// @param dx Rightward movement of the image, in edge units.
    /// @param dy Downward movement of the image, in edge units.
    void moveImageBy(double dx, double dy);

    /// @brief Rotates the image to an angle as it appears on screen, about the crop's centre.
    ///
    /// The angle is clamped to the straighten limits. An explicit crop keeps
    /// the content under its centre and its size, shrinking about that centre
    /// (keeping its aspect) only as far as rotated content requires, and
    /// moving only when nothing fits there (ADR 014, ADR 040); an automatic
    /// crop stays automatic.
    /// @param degrees Clockwise on screen.
    void rotateTo(double degrees);

    /// @brief Straightens along a line drawn on the photograph as shown.
    ///
    /// Rotates so that the line becomes horizontal or vertical, whichever is
    /// nearer, within the straighten limits. A line too short to have a
    /// direction changes nothing.
    void straightenAlong(CropPoint from, CropPoint to);

    /// @brief Sets the aspect constraint.
    ///
    /// Free leaves the rectangle as it is. A ratio fits the largest crop of
    /// that ratio inside the current explicit one, about its centre; an
    /// automatic crop stays automatic and resolves at the new ratio.
    void setAspect(const CropAspect& aspect);

    /// @brief Locks the aspect at the crop's present ratio, or frees it.
    void setLocked(bool locked);

    /// @brief Swaps portrait and landscape.
    ///
    /// A locked ratio is reciprocated. The crop's width and height are swapped
    /// about its centre, then shrunk about it if they no longer fit.
    void swapOrientation();

    /// @brief Turns the photograph by a quarter, as it appears on screen.
    ///
    /// Composes the turn in displayed axes, swapping the flips (ADR 014); the
    /// crop is carried with the content and a ratio reciprocated.
    /// @param clockwise Whether clockwise.
    void turn(bool clockwise);

    /// @brief Mirrors the photograph as it appears on screen, carrying the crop.
    /// @param horizontal Whether left and right swap, rather than top and bottom.
    void flip(bool horizontal);

    /// @brief Returns to automatic framing, keeping the aspect constraint.
    void resetCrop();

    /// @brief Takes a geometry from elsewhere, such as a slider, into the edit.
    ///
    /// A change of straighten alone rotates as rotateTo() does, so the crop
    /// shrinks to fit; anything else is taken as it is, an explicit crop that
    /// leaves valid content being fitted back into it.
    /// @throws std::invalid_argument if the geometry is not valid; nothing changes.
    void adopt(const GeometrySettings& geometry);

    /// @brief Opens a step of the history; what changes until the matching endStep() is one step.
    ///
    /// Steps nest: an inner one joins the outer, so a slider edit made of
    /// many adopted geometries, or a command inside it, is still one step.
    void beginStep();

    /// @brief Closes a step; the outermost records it, if the geometry changed since it opened.
    ///
    /// Recording a step forgets what could be redone. Does nothing with no step open.
    void endStep();

    /// @brief Tells whether a step is open.
    [[nodiscard]] bool inStep() const noexcept {
        return stepDepth_ > 0;
    }

    /// @brief Tells whether there is a step to undo, the open one included if it changed anything.
    [[nodiscard]] bool canUndo() const noexcept;

    /// @brief Tells whether there is an undone step to redo.
    [[nodiscard]] bool canRedo() const noexcept {
        return !redo_.empty();
    }

    /// @brief Returns to the geometry before the latest step, closing an open one first.
    ///
    /// Does nothing when no step was recorded.
    void undo();

    /// @brief Makes the latest undone step again.
    ///
    /// Does nothing when nothing was undone, or once a step was recorded since.
    void redo();

private:
    /// @brief Makes a geometry current, working out what is derived from it.
    void resolve(GeometrySettings geometry);

    /// @brief Rotates to a stored straighten, from the start of the run of rotations.
    void rotateStored(double straighten);

    SourceShape source_;
    GeometrySettings geometry_;
    /// Geometry at beginGesture().
    GeometrySettings gestureStart_;
    /// Geometry before the latest run of rotations; empty when the last change was something else.
    std::optional<GeometrySettings> rotationStart_;
    /// Frame the geometry resolves to.
    CropFrame frame_;
    /// Geometry before each recorded step, oldest first.
    std::vector<GeometrySettings> undo_;
    /// Geometry after each undone step, latest undone last.
    std::vector<GeometrySettings> redo_;
    /// Geometry as the outermost open step began.
    GeometrySettings stepStart_;
    /// Steps open, nested.
    int stepDepth_ = 0;
};

} // namespace arraw::app
