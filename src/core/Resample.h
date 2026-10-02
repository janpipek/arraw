#pragma once

#include <Develop.h>
#include <ImageBuffer.h>

namespace arraw {

/// @brief Resizes developed float pixels to a size, with a separable kernel.
///
/// Two 1-D passes, rows then columns, with the weights computed once per
/// output coordinate. An output pixel centre at (x + 0.5) maps to the source
/// at (x + 0.5) / scale - 0.5 in pixel-centre coordinates, with the scale
/// taken per axis from the two sizes. When shrinking, the kernel is stretched
/// by 1 / scale (both filters), so that every source pixel contributes and
/// nothing aliases. Taps beyond an edge repeat the edge pixel, and the weights
/// of each output coordinate sum to 1.
///
/// Filtering happens in premultiplied alpha, so a transparent pixel's colour
/// cannot leak into its neighbours. A result with alpha 0 or below is
/// transparent black (all four samples 0), which is also what the geometry
/// pass returns for it. Accumulation is in double; samples are stored as float.
///
/// A kernel with negative lobes (Lanczos) rings around hard edges. After each
/// 1-D pass, a channel whose contributing input samples were all at least 0 is
/// clamped at 0; one that saw a negative input is left alone, so the genuine
/// negatives of a camera matrix survive. Values above 1 are never clamped.
///
/// @param source Developed pixels in the working format.
/// @param size Size of the result, at least one pixel per side.
/// @param filter Kernel to resample with.
/// @param opaque Whether every alpha sample of @p source is exactly one, as
/// ::arraw::ResizePlan::opaque says. Then the result is the same, bit for bit,
/// as without the claim, but the pass tracks no transparency and writes alpha
/// one: no range of colours, no translucency flags, no final clamp. A caller
/// that cannot say leaves it `false`, which is always correct; `true` for a
/// source with transparency is not.
/// @return @p source itself, untouched and not copied, if it already has @p size;
/// otherwise a new buffer in the same encoding.
/// @throws std::invalid_argument if @p source is not in the working format or
/// @p size is empty.
[[nodiscard]] ImageBuffer resample(ImageBuffer source, ImageSize size, ResizeFilter filter,
                                   bool opaque = false);

} // namespace arraw
