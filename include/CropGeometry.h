#pragma once

#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImageOrientation.h>

#include <array>
#include <optional>

/// @file
/// @brief The rules of ADR 014 for editing a geometry, as pure functions over
/// ::arraw::GeometrySettings.
///
/// What a turn, a flip, a straighten or an aspect does to an explicit crop,
/// decided once, so that the crop mode, the Develop panel, the command line and
/// Python edit a geometry alike (looks-and-history plan, §1). Each rule takes a
/// geometry and gives the one that follows; none keeps any state, so a history,
/// a gesture under way or a run of rotations is the caller's to keep (the crop
/// mode's is in `app/CropEditing`, ADR 040).
///
/// **The contract.** Given a geometry valid for the photograph, every rule
/// gives one that is too: its straighten within the limits, and an explicit
/// crop well formed, agreeing with a locked aspect to ADR 014's relative 1e-6,
/// and inside valid rotated content. An automatic crop stays automatic unless
/// a rule says otherwise. A rule that reads the photograph's frame refuses a
/// geometry that is not valid for it (std::invalid_argument, as
/// ::arraw::develop does); the rules that do not read it (turned(), flipped(),
/// withRotation(), withCropReset()) do not check it either.
///
/// **Coordinates.** A ::arraw::CropPoint or a ::arraw::CropBox is in the
/// uncropped upright frame of the geometry it goes with, in its edge units,
/// which are the photograph's pixels: the origin top left, x right and y down.
/// That frame is an isotropic scale of what a screen shows, so a widget
/// converts with one factor and an offset.

namespace arraw {

/// @brief Point in the uncropped upright frame, in its edge units (pixels of the photograph).
struct CropPoint {
    /// @brief Distance from the left edge of the frame.
    double x = 0.0;
    /// @brief Distance from the top edge of the frame.
    double y = 0.0;

    friend bool operator==(const CropPoint&, const CropPoint&) = default;
};

/// @brief Axis-aligned rectangle in the uncropped upright frame, in its edge units.
struct CropBox {
    /// @brief Left edge.
    double left = 0.0;
    /// @brief Top edge.
    double top = 0.0;
    /// @brief Width.
    double width = 0.0;
    /// @brief Height.
    double height = 0.0;

    /// @brief Gives the right edge.
    [[nodiscard]] double right() const noexcept {
        return left + width;
    }
    /// @brief Gives the bottom edge.
    [[nodiscard]] double bottom() const noexcept {
        return top + height;
    }
    /// @brief Gives the centre.
    [[nodiscard]] CropPoint centre() const noexcept {
        return {left + width / 2, top + height / 2};
    }

    friend bool operator==(const CropBox&, const CropBox&) = default;
};

/// @brief Size and camera orientation of a decoded photograph: all the rules read of it.
///
/// Nothing needs decoding to make one: ::arraw::readImageMetadata declares
/// both (shapeOf(const ImageMetadata&)). A caller that holds the decoded
/// buffer can take them from it instead (shapeOf(const ImageBuffer&)), which
/// is what the frame a render resolves against is made from. The two agree for
/// every photograph arraw's tests decode. A half-size decode
/// (::arraw::DecodeOptions::halfSize) does not, and must not be passed here:
/// an explicit crop is stored relative to the frame, and a frame of other
/// proportions would move it or break its locked aspect.
struct SourceShape {
    /// @brief Pixel dimensions of the decoded photograph, before its orientation.
    ImageSize size;
    /// @brief Camera orientation, still to be applied to the decoded photograph.
    ImageOrientation orientation = ImageOrientation::Normal;

    friend bool operator==(const SourceShape&, const SourceShape&) = default;
};

/// @brief Gives the shape a photograph declares, without decoding it.
[[nodiscard]] inline SourceShape shapeOf(const ImageMetadata& photo) noexcept {
    return {photo.size, photo.orientation};
}

/// @brief Gives the shape of a decoded photograph.
[[nodiscard]] inline SourceShape shapeOf(const ImageBuffer& decoded) noexcept {
    return {decoded.size(), decoded.orientation()};
}

/// @brief Frame a geometry resolves to on a photograph: what a crop editor draws and measures.
struct CropFrame {
    /// @brief Width of the uncropped upright frame, straighten included.
    double uprightWidth = 0.0;
    /// @brief Height of the uncropped upright frame, straighten included.
    double uprightHeight = 0.0;
    /// @brief Crop as the engine resolves it, automatic framing included.
    CropBox crop;
    /// @brief Corners of the valid content, in order around it.
    ///
    /// The decoded raster's top-left, top-right, bottom-right and bottom-left
    /// corners, mapped: consecutive corners share an edge.
    std::array<CropPoint, 4> content{};
    /// @brief Frame of the photograph turned and flipped but not straightened, at the origin.
    ///
    /// What a render with no straighten and no crop shows (the crop mode
    /// rotates that render on screen, ADR 040), and what the Original aspect
    /// takes its ratio from.
    CropBox unstraightened;

    friend bool operator==(const CropFrame&, const CropFrame&) = default;
};

/// @brief Resolves a geometry's frame on a photograph.
/// @param source Shape of the photograph; not empty.
/// @param geometry Geometry to resolve.
/// @return The upright frame, the resolved crop and the valid content.
/// @throws std::invalid_argument if @p geometry is not valid for @p source, as
/// ::arraw::develop refuses it: a straighten out of its limits, a crop that is
/// not well formed, or one that disagrees with its locked aspect.
[[nodiscard]] CropFrame cropFrameFor(SourceShape source, const GeometrySettings& geometry);

/// @brief Gives the width-over-height ratio an aspect locks a frame's crop to.
/// @param frame Frame the aspect applies to; the Original aspect reads its unstraightened size.
/// @param aspect Aspect constraint.
/// @return The ratio, or nothing when @p aspect is free.
/// @throws std::invalid_argument if @p aspect is a ratio that is not well formed.
[[nodiscard]] std::optional<double> lockedRatio(const CropFrame& frame, const CropAspect& aspect);

/// @brief Gives the straighten as it appears on screen: degrees, clockwise positive.
///
/// The stored angle comes before the flips (ADR 014), so one flip reverses
/// the direction in which it appears.
[[nodiscard]] double displayedStraighten(const GeometrySettings& geometry) noexcept;

/// @brief Gives the stored straighten that appears on screen as an angle.
///
/// The inverse of ::arraw::displayedStraighten for the geometry's flips; not clamped.
/// @param geometry Geometry whose flips decide the sign.
/// @param displayed Angle as it appears on screen, clockwise positive.
[[nodiscard]] double storedStraighten(const GeometrySettings& geometry, double displayed) noexcept;

/// @brief Turns the photograph by a quarter, as it appears on screen, carrying the crop.
///
/// Composes the turn in displayed axes: the stored quarter-turn steps and the
/// two flips swap, since a turn after a flip is the other flip after the turn
/// (ADR 014). An explicit crop keeps the content it selects; a ratio is
/// reciprocated, so the crop keeps its proportions on screen.
/// @param geometry Geometry to turn.
/// @param clockwise Whether clockwise on screen.
[[nodiscard]] GeometrySettings turned(GeometrySettings geometry, bool clockwise) noexcept;

/// @brief Mirrors the photograph as it appears on screen, carrying the crop.
///
/// The flips are in the final upright axes (ADR 014), so the stored flip
/// toggles and an explicit crop is mirrored with the content.
/// @param geometry Geometry to mirror.
/// @param horizontal Whether left and right swap, rather than top and bottom.
[[nodiscard]] GeometrySettings flipped(GeometrySettings geometry, bool horizontal) noexcept;

/// @brief Sets the stored quarter-turn, carrying the crop with the content.
///
/// The rule for setting the `rotation` value itself, as a document or the
/// command line does, rather than turning on screen: the flips are kept, so
/// with one flip a clockwise step of the stored value turns the picture
/// anticlockwise on screen, and the crop follows it either way. A ratio is
/// reciprocated when the frame's sides swap.
/// @param geometry Geometry to change.
/// @param rotation Quarter-turn to store.
[[nodiscard]] GeometrySettings withRotation(GeometrySettings geometry,
                                            QuarterTurn rotation) noexcept;

/// @brief Straightens to a stored angle from a baseline, about the crop's centre.
///
/// The angle is clamped to the straighten limits. An explicit crop keeps the
/// content under its centre and its size, shrinking about that centre
/// (keeping its aspect) only as far as rotated content requires, and moving
/// only when nothing fits there (ADR 014, ADR 040). An automatic crop stays
/// automatic.
///
/// The result depends on @p baseline only, never on a previous result, so a
/// caller that keeps the geometry from before a run of rotations (a drag, a
/// slider, a typed angle) and passes it every time gets the shrinking undone
/// when the angle goes back. A single edit passes the geometry as it is.
/// @param source Shape of the photograph.
/// @param baseline Geometry the rotation starts from; its straighten is replaced.
/// @param straighten Stored angle, before the flips (see ::arraw::storedStraighten).
/// @return @p baseline straightened, its crop fitted.
/// @throws std::invalid_argument if @p straighten is not finite, or @p baseline is not
/// valid for @p source.
[[nodiscard]] GeometrySettings rotatedTo(SourceShape source, const GeometrySettings& baseline,
                                         double straighten);

/// @brief Straightens along a line drawn on the photograph as shown.
///
/// Rotates, as ::arraw::rotatedTo does from @p geometry, so that the line
/// becomes horizontal or vertical, whichever is nearer, within the straighten
/// limits. A line too short to have a direction changes nothing.
/// @param source Shape of the photograph.
/// @param geometry Geometry as it is; the points are in its upright frame.
/// @param from One end of the line.
/// @param to The other end.
/// @throws std::invalid_argument if @p geometry is not valid for @p source.
[[nodiscard]] GeometrySettings straightenedAlong(SourceShape source,
                                                 const GeometrySettings& geometry, CropPoint from,
                                                 CropPoint to);

/// @brief Sets the aspect constraint.
///
/// Free leaves the rectangle as it is. A ratio, or the Original aspect, fits
/// the largest crop of that ratio inside the explicit one, about its centre;
/// an automatic crop stays automatic and resolves at the new ratio.
/// @param source Shape of the photograph.
/// @param geometry Geometry to change.
/// @param aspect Aspect to set.
/// @throws std::invalid_argument if @p aspect is a ratio that is not well formed, or
/// @p geometry is not valid for @p source.
[[nodiscard]] GeometrySettings withAspect(SourceShape source, GeometrySettings geometry,
                                          const CropAspect& aspect);

/// @brief Locks the aspect at the crop's present ratio.
///
/// An explicit crop's ratio is taken from its stored edges, so it agrees with
/// the lock exactly; an automatic crop's from its resolved size. An aspect
/// already locked, Original or a ratio, is kept. Unlocking is
/// ::arraw::withAspect with ::arraw::FreeCropAspect.
/// @param source Shape of the photograph.
/// @param geometry Geometry whose aspect to lock.
/// @throws std::invalid_argument if @p geometry is not valid for @p source.
[[nodiscard]] GeometrySettings withLockedAspect(SourceShape source, GeometrySettings geometry);

/// @brief Swaps portrait and landscape.
///
/// A locked ratio is reciprocated; reciprocating the Original aspect's ratio
/// gives a ratio, and reciprocating that gives the Original aspect back. The
/// crop's width and height are swapped about its centre, then shrunk about it
/// if they no longer fit; an automatic crop with a locked aspect stays
/// automatic, and one that is free becomes explicit.
/// @param source Shape of the photograph.
/// @param geometry Geometry whose orientation to swap.
/// @throws std::invalid_argument if @p geometry is not valid for @p source.
[[nodiscard]] GeometrySettings withSwappedOrientation(SourceShape source,
                                                      GeometrySettings geometry);

/// @brief Returns to automatic framing, keeping the aspect constraint.
/// @param geometry Geometry whose crop to reset.
[[nodiscard]] GeometrySettings withCropReset(GeometrySettings geometry) noexcept;

/// @brief Moves the crop by a distance, sliding along valid content rather than leaving it.
///
/// The resolved crop is moved and then put the shortest distance back inside
/// valid content, keeping its size wherever it fits. The result is explicit,
/// with the aspect kept.
/// @param source Shape of the photograph.
/// @param geometry Geometry whose crop moves.
/// @param dx Rightward movement of the crop, in edge units.
/// @param dy Downward movement of the crop, in edge units.
/// @throws std::invalid_argument if @p geometry is not valid for @p source.
[[nodiscard]] GeometrySettings withCropMovedBy(SourceShape source, GeometrySettings geometry,
                                               double dx, double dy);

/// @brief Fits an explicit crop back inside valid content.
///
/// A crop already inside, to a slack for rounding, is returned exactly as it
/// is. One that is not becomes the crop the engine would resolve it to: shrunk
/// about its centre, keeping its aspect, or moved when nothing fits there
/// (ADR 014). An automatic crop is returned as it is. A crop that disagrees
/// with its locked aspect is refused, not reshaped: ::arraw::withAspect is the
/// rule that reshapes a crop to a ratio.
/// @param source Shape of the photograph.
/// @param geometry Geometry whose crop is fitted.
/// @throws std::invalid_argument if @p geometry is not valid for @p source in any other way
/// than its crop lying outside valid content.
[[nodiscard]] GeometrySettings fittedCrop(SourceShape source, GeometrySettings geometry);

} // namespace arraw
