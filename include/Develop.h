#pragma once

#include <DevelopState.h>
#include <ImageBuffer.h>
#include <RenderCheckpoint.h>

#include <cstdint>
#include <optional>
#include <variant>

namespace arraw {

/// @brief Whether a render may enlarge a photograph beyond its own resolution.
enum class Upscale {
    Never,   ///< A request larger than the source is satisfied by the source.
    Allowed, ///< The photograph is interpolated up to the requested size.
};

/// @brief Resampling kernel for a render to a requested size.
enum class ResizeFilter {
    Lanczos3, ///< Windowed sinc of radius 3: sharp, with a little ringing at hard edges.
    Bilinear, ///< Tent kernel: soft, never rings. Widened when shrinking, so it does not alias.
};

/// @brief Level of effort a render spends on being exact.
enum class Quality {
    Export, ///< Full resolution all the way, the result an export would write.
};

/// @brief Named position inside the pointwise chain whose values a render can hand back.
///
/// A tap is not a pass boundary: inside the fused pointwise chain a colour is
/// in registers, so there is no buffer to keep (ADR 011). ::arraw::sample asks
/// for one by running the render with the chain stopped there. Each tap names
/// the encoding its samples come back in. The position of each lives in
/// `src/core/ProcessingPlan.h`, beside the chain itself.
enum class Tap {
    /// What the tone curves take in: after white balance, the matrix, exposure
    /// and Basic Tone, before the curves, the shoulder and the colour controls.
    /// Handed back in ::arraw::perceptualEncoding, the coordinate the curves
    /// act in, which is the x-axis of a curve widget (ADR 010, ADR 035).
    CurveInput,
};

/// @brief What a caller wants rendered.
///
/// Defaults to the whole photograph at its own resolution. Both a preview and
/// an export at a chosen size are the same request, which is what keeps them
/// from drifting apart (ADR 007).
struct RenderRequest {
    /// @brief Box to fit the result inside, keeping the aspect ratio.
    ///
    /// A long edge of N is `FitInside{N, N}`.
    struct FitInside {
        std::uint32_t width = 0;  ///< Largest width of the result, in pixels.
        std::uint32_t height = 0; ///< Largest height of the result, in pixels.
    };

    /// @brief Factor to multiply both sides of the cropped photograph by.
    struct Scale {
        double factor = 1.0; ///< Multiplier, finite and positive.
    };

    /// @brief Rectangle of the developed frame to render, in fractions of its sides.
    ///
    /// Measured on the frame after geometry (orientation, rotation and crop),
    /// from its top-left corner: `{0, 0, 1, 1}` is all of it.
    struct Region {
        double left = 0.0;   ///< Left edge, from 0 inclusive.
        double top = 0.0;    ///< Top edge, from 0 inclusive.
        double right = 1.0;  ///< Right edge, above @ref left and at most 1.
        double bottom = 1.0; ///< Bottom edge, above @ref top and at most 1.
    };

    /// @brief Size of the result relative to the cropped photograph, or to the region.
    ///
    /// Empty keeps the photograph's own resolution (the region's, if there is
    /// one). See ::arraw::resolvedSize.
    std::optional<std::variant<FitInside, Scale>> size = std::nullopt;

    /// @brief Part of the developed frame to render, or empty for all of it.
    ///
    /// The region is cut from the frame after geometry, snapped outward to whole
    /// pixels (at least one), and the @ref size then resolves against the
    /// region's pixel size: the result is that part of the frame, at that
    /// scale. A region covering the frame is the same render as none. It is
    /// part of the resize stage (ADR 025), so a checkpoint taken after geometry
    /// serves every region, and moving the region redoes only the last stage.
    /// Each side must be finite, with `0 <= left < right <= 1` and
    /// `0 <= top < bottom <= 1`; anything else is refused with
    /// `std::invalid_argument` when the render is planned.
    std::optional<Region> region = std::nullopt;

    /// @brief Whether a size larger than the photograph enlarges it.
    Upscale upscale = Upscale::Never;

    /// @brief Kernel used when the size changes.
    ResizeFilter filter = ResizeFilter::Lanczos3;

    /// @brief Effort the render spends on exactness.
    Quality quality = Quality::Export;
};

/// @brief Gives the part of a frame a request's region actually renders.
///
/// A region is snapped outward to whole pixels of the frame it is cut from
/// (ADR 025), so what is rendered can be a little larger than what was asked
/// for, and differs between frames of different sizes, such as two pyramid
/// levels. A caller placing the result on screen needs this, not the request.
/// @param request Request whose region is resolved; all of the frame when it has none.
/// @param frame Size of the developed, cropped frame the region is cut from.
/// @return The rendered part, in normalised coordinates of @p frame.
/// @throws std::invalid_argument if the region is invalid or @p frame is empty.
[[nodiscard]] RenderRequest::Region renderedRegion(const RenderRequest& request, ImageSize frame);

/// @brief Resolves a request's size against the size of what is resized.
///
/// That is the photograph's size after its crop, or the size of the request's
/// ::arraw::RenderRequest::region in pixels when it has one; this function does
/// not look at the region, so a caller passes the size it has cut.
///
/// The scale is the factor in a ::arraw::RenderRequest::Scale, or for a
/// ::arraw::RenderRequest::FitInside the
/// smallest that fits both sides in the box. Unless the request allows
/// upscaling the scale is capped at 1. Each side is the cropped side times the
/// scale, rounded, and at least one pixel (and, for a box, at most the box).
/// This is the one place sizes are worked out, so that every backend and
/// caller agrees.
/// @param request What the caller wants rendered.
/// @param cropped Size of the frame (or region) the request is applied to.
/// @return The size of the rendered result; @p cropped if the request has no size.
/// @throws std::invalid_argument if @p cropped is empty, a box has a zero side,
/// a factor is not finite and positive, or the result does not fit a 32-bit side.
[[nodiscard]] ImageSize resolvedSize(const RenderRequest& request, ImageSize cropped);

/// @brief Gives the size of the developed frame at the source's own resolution.
///
/// The size after orientation, rotation and crop: the frame a
/// ::arraw::RenderRequest::region is a fraction of, and the one a size resolves
/// against when the request has no region.
/// @param sourceSize Size of the decoded photograph.
/// @param orientation Camera orientation of the decoded photograph.
/// @param state How the photograph is developed.
/// @return The size of the frame; never empty for a non-empty source.
/// @throws std::invalid_argument as ::arraw::develop does for its geometry.
[[nodiscard]] ImageSize croppedSize(ImageSize sourceSize, ImageOrientation orientation,
                                    const DevelopState& state);

/// @brief Renders a decoded photograph through its develop state.
///
/// The source may be in the working encoding, as anything Qt decoded is, or in
/// a camera's own primaries, as a RAW is. Either way the result is in the
/// working encoding, at ::arraw::workingFormat: the conversion out of camera
/// space is part of developing, not of decoding, because a white balance is
/// only a white balance in the space the sensor recorded (ADR 007).
///
/// With default settings a RAW is converted faithfully and its brightest
/// values are rolled toward white: the shoulder is a stage of the chain rather
/// than an effect a photographer switches on, because a real multiply puts
/// values above white and something has to bring them back (ADR 010). Below
/// its knee nothing else happens.
///
/// Camera orientation and user geometry are applied after colour and tone,
/// with one bilinear resample in linear working colour and premultiplied alpha.
/// Crops are constrained to valid image content. The output has no pending
/// camera orientation. Exact quarter-turns and pixel-aligned crops copy samples.
/// A requested size is then resolved against the cropped size and applied as a
/// separate resize with the request's filter; a size equal to the cropped one
/// leaves the pixels untouched. A request's region is cut from the cropped
/// frame just before that resize, and the size resolves against the cut.
///
/// @param source Decoded photograph, in the working or a camera encoding.
/// @param state How the photograph is developed.
/// @param request What to render; the default is the whole photograph at its
/// own resolution.
/// @return A new buffer in the working encoding.
/// @throws std::invalid_argument if @p source is in an encoding development
/// cannot start from, if geometry is invalid, or if @p request is invalid.
[[nodiscard]] ImageBuffer develop(const ImageBuffer& source, const DevelopState& state,
                                  const RenderRequest& request = {});

/// @brief Develops a decoded photograph as far as a pass boundary and keeps what is there.
///
/// The CPU's `stopAfter` (ADR 011): the same stages as ::arraw::develop, in the
/// same order and from the same plan, stopped after one. The result is a host
/// checkpoint a later ::arraw::resumeFrom can carry on from, so that an edit
/// that only touches later stages does not pay for the earlier ones.
///
/// The request is read only when @p stopAfter is ::arraw::Stage::Resize; earlier
/// stops ignore it, whatever it says, as the GPU does.
/// @param source Decoded photograph, in the working or a camera encoding.
/// @param state How the photograph is developed.
/// @param stopAfter Last boundary to run. ::arraw::Stage::Pointwise leaves a
/// result of the source's size with no geometry applied, ::arraw::Stage::Geometry
/// one with no resize.
/// @param request What to render; see ::arraw::develop.
/// @return A host checkpoint in the working encoding, with no pending orientation.
/// @throws std::invalid_argument as ::arraw::develop, and if @p stopAfter is not
/// a boundary.
[[nodiscard]] RenderCheckpoint developUntil(const ImageBuffer& source, const DevelopState& state,
                                            Stage stopAfter, const RenderRequest& request = {});

/// @brief Carries a render on from a checkpoint, stopping after a boundary.
///
/// Valid only if the plan this render resolves equals the checkpoint's up to the
/// checkpoint's own boundary (ADR 011), so a tone edit refuses a pointwise
/// checkpoint, a viewport change reuses a geometry one, and a checkpoint of
/// another size, such as one from another pyramid level, is refused. Nothing
/// about the plan is exposed: the engine compares, the caller holds. What it
/// cannot tell apart is two sources of one size and encoding, since the plan has
/// no decode block yet (ADR 012); a caller that changes the pixels under it
/// must drop its checkpoints.
///
/// Resuming costs a copy of the checkpoint's pixels, because the payload is
/// shared and immutable and the stages take their input by value. It is a
/// fraction of the passes it skips; ::arraw::develop itself pays none.
///
/// Resuming at the checkpoint's own boundary returns @p from itself, which is
/// equivalent and shares its pixels.
/// @param from Checkpoint to resume from; a host one.
/// @param source Decoded photograph the checkpoint was made from. Still read,
/// for what planning needs.
/// @param state How the photograph is developed now.
/// @param stopAfter Last boundary to run; not before the checkpoint's.
/// @param request What to render; read only when @p stopAfter is ::arraw::Stage::Resize.
/// @return A host checkpoint at @p stopAfter, equal to what ::arraw::developUntil
/// of the same arguments gives, bit for bit.
/// @throws std::invalid_argument if @p from is resident on a device, its plan
/// prefix or pixels do not match this render, @p stopAfter is not a boundary or
/// is before the checkpoint's, or as ::arraw::develop.
[[nodiscard]] RenderCheckpoint resumeFrom(const RenderCheckpoint& from, const ImageBuffer& source,
                                          const DevelopState& state, Stage stopAfter,
                                          const RenderRequest& request = {});

/// @brief Renders a photograph with the pointwise chain stopped at a tap, to measure it.
///
/// ADR 011's `sample(tap)`, the looking verb beside ::arraw::developUntil's
/// continuing one: it hands back pixels and no checkpoint, because a tap is
/// inside a pass. The chain writes the colour at @p tap instead of the
/// developed one; geometry, the region and the resize then run as for
/// ::arraw::develop, in linear light, so the result covers the same frame at the
/// same size as a render of the same request would. Last, the colour is
/// encoded into the tap's encoding. A preview's reduced source therefore gives
/// preview-resolution samples, and the crop decides which pixels are measured.
///
/// Alpha passes through as it does in a render. Nothing earlier can be reused:
/// the only boundary before a tap is the source itself (ADR 035).
/// @param source Decoded photograph, in the working or a camera encoding.
/// @param state How the photograph is developed.
/// @param tap Where in the chain to stop.
/// @param request What to render; see ::arraw::develop.
/// @return A new ::arraw::workingFormat buffer in the encoding @p tap names
/// (::arraw::perceptualEncoding for ::arraw::Tap::CurveInput), with no pending
/// orientation.
/// @throws std::invalid_argument as ::arraw::develop, and if @p tap is not a tap.
[[nodiscard]] ImageBuffer sample(const ImageBuffer& source, const DevelopState& state, Tap tap,
                                 const RenderRequest& request = {});

} // namespace arraw
