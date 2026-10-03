#pragma once

#include <Develop.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>

namespace arraw {

/// @brief Halves an image in both directions by averaging 2x2 blocks.
///
/// The result is ::arraw::PixelFormat::RgbaF32 (an image in another format is
/// converted first, as development does), with the input's encoding and pending
/// orientation, and `ceil(w / 2)` by `ceil(h / 2)` pixels. An odd last row or
/// column averages the samples it has: nothing is padded or wrapped.
///
/// Colour is averaged premultiplied by alpha and divided by the mean alpha
/// afterwards, so a transparent pixel does not darken its neighbours; a block
/// with no alpha at all becomes transparent black. For an opaque image this is
/// the plain mean of the colours, and alpha stays exactly 1.
///
/// Repeated, it makes the pyramid of reductions a preview develops from
/// (ADR 020): each level halves the one before.
/// @param image Image to halve.
/// @return A new image of half the size.
/// @throws std::invalid_argument if @p image is 1x1, as there is nothing to
/// halve.
[[nodiscard]] ImageBuffer halved(const ImageBuffer& image);

/// @brief Chooses the pyramid level a render needs.
///
/// Level 0 is the source itself and each level halves the one before (see
/// ::arraw::halved). The answer is the largest level whose cropped size,
/// `ceil(cropped / 2^level)` per side, still covers the size the request
/// resolves to on both sides: at most twice the pixels needed, and never fewer.
/// A request that enlarges, that keeps the cropped size, or that is as large
/// as it, needs level 0.
///
/// With a ::arraw::RenderRequest::region the size compared is the region's
/// pixel size at that level, since that is what is resized: a closer view
/// needs a finer level for the same output.
///
/// The cropped size is the one at full resolution, from the same geometry
/// plan a render makes, so orientation, rotation and crop are accounted for.
/// The caller develops that level with the same request.
/// @param sourceSize Size of the full-resolution source, before orientation.
/// @param orientation Orientation still to be applied to the source.
/// @param state How the photograph is developed; only its geometry is read.
/// @param request What is to be rendered.
/// @return The level, 0 or more.
/// @throws std::invalid_argument as ::arraw::geometryPlanFor and
/// ::arraw::resolvedSize do.
[[nodiscard]] int pyramidLevelFor(ImageSize sourceSize, ImageOrientation orientation,
                                  const DevelopState& state, const RenderRequest& request);

} // namespace arraw
