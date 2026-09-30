#pragma once

#include "GpuContext.h"

#include <DevelopSettings.h>
#include <ImageBuffer.h>
#include <RenderCheckpoint.h>

namespace arraw {

/// @brief Develops a decoded photograph on a device, stopping after one boundary.
///
/// The GPU counterpart of ::arraw::develop, from the same plan: the pointwise
/// chain in one pass, then the geometry resample in another, skipped under
/// exactly the condition the CPU skips it. The source is converted to RGBA
/// float on the host first, by the conversion development itself uses, and
/// uploaded; the result stays on the device until someone reads it back.
///
/// Never falls back to the CPU. Whether a failure here should be retried
/// there is the caller's policy, and a fallback inside would hide a missing
/// GPU (ADR 015).
/// @param context Device to develop on; used from its owner thread only.
/// @param source Decoded photograph, in the working or a camera encoding.
/// @param settings Photographic settings to apply.
/// @param stopAfter Last boundary to run: ::arraw::Stage::Pointwise leaves a
/// result of the source's size with no geometry applied.
/// @return A resident checkpoint in the working encoding, with no pending
/// orientation.
/// @throws std::invalid_argument if development cannot start from @p source,
/// the settings cannot be resolved, @p stopAfter is not a boundary, or an image
/// is larger than the device accepts.
/// @throws std::logic_error if called from a thread other than the context's owner.
/// @throws std::runtime_error if the device cannot do the work or fails.
[[nodiscard]] RenderCheckpoint developOnGpu(GpuContext& context, const ImageBuffer& source,
                                            const DevelopSettings& settings,
                                            Stage stopAfter = Stage::Geometry);

} // namespace arraw
