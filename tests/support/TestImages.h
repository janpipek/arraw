#pragma once

#include "ImageBuffer.h"

namespace arraw::test {

/// @brief Builds a saturated hue sweep, for tests that need recognisable pixels.
///
/// Hue runs left to right over the full circle and brightness runs top to
/// bottom, so the result is asymmetric in both axes: a flipped, rotated, or
/// row-misaligned copy cannot be mistaken for the original. Alpha, where the
/// format has it, is opaque.
/// @param size Pixel dimensions of the image.
/// @param format Sample layout to produce.
/// @param encoding Meaning to record for the sample values.
/// @return A freshly allocated buffer holding the sweep.
[[nodiscard]] ImageBuffer rainbow(ImageSize size = {32, 32},
                                  PixelFormat format = PixelFormat::RgbaU8,
                                  ColorEncoding encoding = ColorEncoding::Srgb);

} // namespace arraw::test
