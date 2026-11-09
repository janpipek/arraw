#pragma once

#include <ColorEncoding.h>
#include <ImageBuffer.h>

#include <QImage>

namespace arraw {

/// @brief Converts a buffer's colours to an output encoding, in the layout asked for.
///
/// The conversion an export makes before it writes, shared with the output histogram and the
/// display so that they cannot disagree. Computed here, not by Qt (ADR 057): the source's curve to
/// linear, the change of primaries, the target's curve, and one rounding. Integer layouts clamp to
/// [0, 1] per channel; `Format_RGBA32FPx4` does not, and extends the curves odd below zero. Alpha
/// stays straight and is only rounded to the depth. The image carries the target's colour space.
/// @param image Buffer in a named encoding, in a layout QImage can wrap.
/// @param encoding Target encoding.
/// @param format QImage layout of the result: RGB888, RGBA8888, RGBX64, RGBA64 or RGBA32FPx4.
/// @return The converted image.
/// @throws std::invalid_argument if @p image is camera-native or has no QImage layout, or
/// @p format is not one of those above.
/// @throws std::runtime_error if the result cannot be allocated.
[[nodiscard]] QImage encodedForOutput(const ImageBuffer& image, NamedEncoding encoding,
                                      QImage::Format format);

} // namespace arraw
