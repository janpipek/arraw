#pragma once

#include "GpuContext.h"

#include <Develop.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <RenderCheckpoint.h>

namespace arraw {

/// @brief Develops a decoded photograph on a device, stopping after one boundary.
///
/// The GPU counterpart of ::arraw::develop, from the same plan: the pointwise
/// chain in one pass, then the geometry resample in another, skipped under
/// exactly the condition the CPU skips it, then the resize to the requested
/// size (horizontal, then vertical; see `Resample.cpp`; two renders for an opaque
/// source, four otherwise), skipped when the size is the cropped one. The source is converted to
/// RGBA float on the host first, by the conversion development itself uses, and uploaded; the
/// result stays on the device until someone reads it back.
///
/// Never falls back to the CPU. Whether a failure here should be retried
/// there is the caller's policy, and a fallback inside would hide a missing
/// GPU (ADR 015).
/// @param context Device to develop on; used from its owner thread only.
/// @param source Decoded photograph, in the working or a camera encoding.
/// @param state How the photograph is developed.
/// @param stopAfter Last boundary to run: ::arraw::Stage::Pointwise leaves a
/// result of the source's size with no geometry applied, and
/// ::arraw::Stage::Geometry one with no resize, whatever @p request asks.
/// @param request Size and filter to render at; the default is the cropped size
/// of the photograph, which costs no pass. Only its size, upscale and filter
/// are read.
/// @return A resident checkpoint in the working encoding, with no pending
/// orientation.
/// @throws std::invalid_argument if development cannot start from @p source,
/// the settings cannot be resolved, @p stopAfter is not a boundary, @p request
/// cannot be resolved (see ::arraw::resolvedSize; checked only when
/// @p stopAfter is ::arraw::Stage::Resize, as earlier stops ignore it), or an
/// image is larger than the device accepts.
/// @throws std::logic_error if called from a thread other than the context's owner.
/// @throws std::runtime_error if the device cannot do the work or fails.
[[nodiscard]] RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                                            const DevelopState& state,
                                            Stage stopAfter = Stage::Resize,
                                            const RenderRequest& request = {});

} // namespace arraw
