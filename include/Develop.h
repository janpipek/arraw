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

    /// @brief Size of the result relative to the cropped photograph.
    ///
    /// Empty keeps the photograph's own resolution. See ::arraw::resolvedSize.
    std::optional<std::variant<FitInside, Scale>> size = std::nullopt;

    /// @brief Whether a size larger than the photograph enlarges it.
    Upscale upscale = Upscale::Never;

    /// @brief Kernel used when the size changes.
    ResizeFilter filter = ResizeFilter::Lanczos3;

    /// @brief Effort the render spends on exactness.
    Quality quality = Quality::Export;
};

/// @brief Resolves a request's size against the photograph's size after its crop.
///
/// The scale is the factor in a ::arraw::RenderRequest::Scale, or for a
/// ::arraw::RenderRequest::FitInside the
/// smallest that fits both sides in the box. Unless the request allows
/// upscaling the scale is capped at 1. Each side is the cropped side times the
/// scale, rounded, and at least one pixel (and, for a box, at most the box).
/// This is the one place sizes are worked out, so that every backend and
/// caller agrees.
/// @param request What the caller wants rendered.
/// @param cropped Size of the photograph after its crop.
/// @return The size of the rendered result; @p cropped if the request has no size.
/// @throws std::invalid_argument if @p cropped is empty, a box has a zero side,
/// a factor is not finite and positive, or the result does not fit a 32-bit side.
[[nodiscard]] ImageSize resolvedSize(const RenderRequest& request, ImageSize cropped);

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
/// leaves the pixels untouched.
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

} // namespace arraw
