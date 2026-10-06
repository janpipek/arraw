#pragma once

#include "GpuContext.h"

#include <Develop.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <Progress.h>
#include <RenderCheckpoint.h>

namespace arraw {

/// @brief Uploads a decoded photograph in the form ::arraw::developOnGpu reads it.
///
/// Converts @p source to RGBA float on the host first, by the conversion
/// development itself uses, unless it already is. A caller that renders the
/// same photograph repeatedly uploads it once and develops from the result,
/// instead of paying for the transfer on every render.
/// @param context Device to upload to; used from its owner thread only.
/// @param source Decoded photograph, in the working or a camera encoding.
/// @return The photograph on @p context's device, to pass to developOnGpu.
/// Belongs to the context's owner thread, as every device image does.
/// @throws std::invalid_argument if @p source cannot be converted, or is larger
/// than the device or a single transfer accepts.
/// @throws std::logic_error if called from a thread other than the context's owner.
/// @throws std::runtime_error if the device cannot do the work or fails.
[[nodiscard]] DeviceImage uploadSource(GpuContext& context, const ImageBuffer& source);

/// @brief Develops a decoded photograph on a device, stopping after one boundary.
///
/// The GPU counterpart of ::arraw::develop, from the same plan: the pointwise
/// chain in one pass, then the geometry resample in another, skipped under
/// exactly the condition the CPU skips it, then the resize to the requested
/// size (horizontal, then vertical; see `Resample.cpp`; two renders for an opaque
/// source, four otherwise), skipped when the size is the cropped one, then the
/// effects in one pass, skipped when every effect is off. The source is converted to
/// RGBA float on the host first, by the conversion development itself uses, and uploaded; the
/// result stays on the device until someone reads it back.
///
/// Never falls back to the CPU. Whether a failure here should be retried
/// there is the caller's policy, and a fallback inside would hide a missing
/// GPU (ADR 015).
/// @param context Device to develop on; used from its owner thread only.
/// @param source Decoded photograph, in the working or a camera encoding.
/// @param state How the photograph is developed.
/// @param stopAfter Last boundary to run: ::arraw::Stage::Denoise leaves the
/// source after noise reduction, in its own encoding and pending orientation
/// (the upload itself, shared, when noise reduction is off);
/// ::arraw::Stage::Pointwise a result of the source's size with no geometry applied, and
/// ::arraw::Stage::Geometry one with no resize, whatever @p request asks, and
/// ::arraw::Stage::Resize one with no effects.
/// @param request Size and filter to render at; the default is the cropped size
/// of the photograph, which costs no pass. Only its size, upscale and filter
/// are read.
/// @param progress Channel for progress and cancellation (ADR 042), or null: a
/// render at a time, the cancellation noticed between renders, never inside one.
/// @return A resident checkpoint; in the working encoding, with no pending
/// orientation, at every boundary after ::arraw::Stage::Denoise.
/// @throws std::invalid_argument if development cannot start from @p source,
/// the settings cannot be resolved, @p stopAfter is not a boundary, @p request
/// cannot be resolved (see ::arraw::resolvedSize; checked only when
/// @p stopAfter is ::arraw::Stage::Resize or later, as earlier stops ignore it), or an
/// image is larger than the device accepts.
/// @throws std::logic_error if called from a thread other than the context's owner.
/// @throws std::runtime_error if the device cannot do the work or fails.
/// @throws ::arraw::Cancelled if @p progress was cancelled before the work finished;
/// nothing it made is kept.
[[nodiscard]] RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                                            const DevelopState& state,
                                            Stage stopAfter = Stage::Effects,
                                            const RenderRequest& request = {},
                                            ProgressChannel* progress = nullptr);

/// @brief Develops a photograph already on the device, stopping after one boundary.
///
/// As the overload above, which is this after ::arraw::uploadSource. The host
/// @p source is still read, for what planning needs: its encoding, size,
/// orientation and, for a resize, whether it is opaque.
/// @param context Device to develop on; used from its owner thread only.
/// @param source Decoded photograph, the one @p uploaded was made from.
/// @param uploaded Result of ::arraw::uploadSource of @p source on @p context;
/// left untouched, so it serves any number of renders.
/// @param state How the photograph is developed.
/// @param stopAfter Last boundary to run; see the overload above.
/// @param request Size and filter to render at; see the overload above.
/// @param progress Channel for progress and cancellation (ADR 042), or null: a
/// render at a time, the cancellation noticed between renders, never inside one.
/// @return A resident checkpoint; in the working encoding, with no pending
/// orientation, at every boundary after ::arraw::Stage::Denoise.
/// @throws std::invalid_argument as the overload above, and if @p uploaded is
/// empty, belongs to another device than @p context, or is not of @p source's size.
/// @throws std::logic_error if called from a thread other than the context's owner.
/// @throws std::runtime_error if the device cannot do the work or fails.
/// @throws ::arraw::Cancelled if @p progress was cancelled before the work finished;
/// nothing it made is kept.
RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                              const DeviceImage& uploaded, const DevelopState& state,
                              Stage stopAfter = Stage::Effects, const RenderRequest& request = {},
                              ProgressChannel* progress = nullptr);

/// @brief Carries a render on from a checkpoint that is on this device.
///
/// Runs only the passes after the checkpoint's boundary, from the same plan and
/// through the same code as the overloads above, so a resumed render is the
/// same as a fresh one, with fewer passes (see ::arraw::GpuContext::renderCount).
/// Valid only under ADR 011's rule: the plan this render resolves must equal the
/// checkpoint's up to its boundary, and the pixels must be of the size it would
/// have made. Two sources of one size and encoding are not told apart (see
/// ::arraw::resumeFrom), so a caller that changes the source drops its
/// checkpoints.
///
/// A checkpoint in host memory is refused rather than uploaded: the transfer
/// would be a full-size cost that this call's callers, who hold resident
/// checkpoints to avoid exactly that, did not ask for. Resuming at the
/// checkpoint's own boundary returns @p from itself.
/// @param context Device to develop on; the one that holds @p from; used from
/// its owner thread only.
/// @param from Resident checkpoint made on @p context.
/// @param source Decoded photograph the checkpoint was made from. Read for what
/// planning needs: its encoding, size, orientation and, for a resize, whether it
/// is opaque.
/// @param state How the photograph is developed now.
/// @param stopAfter Last boundary to run; not before the checkpoint's.
/// @param request Size and filter to render at; read only when @p stopAfter is
/// ::arraw::Stage::Resize or later.
/// @param progress Channel for progress and cancellation (ADR 042), or null: a
/// render at a time, the cancellation noticed between renders, never inside one.
/// @return A resident checkpoint at @p stopAfter.
/// @throws std::invalid_argument if @p from is in host memory, belongs to another
/// device, or does not match this render (plan prefix or size), @p stopAfter is not
/// a boundary or is before the checkpoint's, or as the overloads above.
/// @throws std::logic_error if called from a thread other than the context's owner.
/// @throws std::runtime_error if the device cannot do the work or fails.
/// @throws ::arraw::Cancelled if @p progress was cancelled before the work finished;
/// nothing it made is kept.
[[nodiscard]] RenderCheckpoint developOnGpu(GpuContext& context, const RenderCheckpoint& from,
                                            const ImageBuffer& source, const DevelopState& state,
                                            Stage stopAfter = Stage::Effects,
                                            const RenderRequest& request = {},
                                            ProgressChannel* progress = nullptr);

/// @brief Samples a photograph already on the device at a tap, and reads the result back.
///
/// The GPU's ::arraw::sample: the pointwise pass writes the tap's colour
/// through its probe (::arraw::probeFor) instead of the developed one, the
/// geometry and resize passes run on it as for a render to @p request, and the
/// result is read back and encoded on the host by the same function the CPU
/// uses. No checkpoint is made: a tap is inside a pass (ADR 011, ADR 035).
/// @param context Device to sample on; used from its owner thread only.
/// @param source Decoded photograph, the one @p uploaded was made from; read
/// for what planning needs.
/// @param uploaded Result of ::arraw::uploadSource of @p source on @p context.
/// @param state How the photograph is developed.
/// @param tap Where in the chain to stop.
/// @param request What to render; see ::arraw::develop. A sample meant for
/// ::arraw::curveHistogram passes ::arraw::curveHistogramRequest: the request is
/// honoured as given, and the default is a full-resolution Lanczos sample.
/// @param progress Channel for progress and cancellation (ADR 042), or null: a
/// render at a time, the cancellation noticed between renders, never inside one.
/// @return A host buffer, as ::arraw::sample of the same arguments gives within the
/// parity tolerance of the passes it ran.
/// @throws std::invalid_argument if @p tap is not a tap, @p uploaded does not
/// belong to @p context or is not of @p source's size, or as ::arraw::developOnGpu.
/// @throws std::logic_error if called from a thread other than the context's owner.
/// @throws std::runtime_error if the device cannot do the work or fails.
/// @throws ::arraw::Cancelled if @p progress was cancelled before the work finished;
/// nothing it made is kept.
[[nodiscard]] ImageBuffer sampleOnGpu(GpuContext& context, const ImageBuffer& source,
                                      const DeviceImage& uploaded, const DevelopState& state,
                                      Tap tap, const RenderRequest& request = {},
                                      ProgressChannel* progress = nullptr);

/// @brief Uploads a decoded photograph, samples it at a tap, and reads the result back.
///
/// As the overload above, after ::arraw::uploadSource.
/// @param context Device to sample on; used from its owner thread only.
/// @param source Decoded photograph, in the working or a camera encoding.
/// @param state How the photograph is developed.
/// @param tap Where in the chain to stop.
/// @param request What to render; see ::arraw::develop.
/// @param progress Channel for progress and cancellation (ADR 042), or null: a
/// render at a time, the cancellation noticed between renders, never inside one.
/// @return A host buffer in the tap's encoding.
/// @throws As the overload above.
[[nodiscard]] ImageBuffer sampleOnGpu(GpuContext& context, const ImageBuffer& source,
                                      const DevelopState& state, Tap tap,
                                      const RenderRequest& request = {},
                                      ProgressChannel* progress = nullptr);

} // namespace arraw
