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

/// @brief Rounds a float image that is already in its output encoding to a layout.
///
/// The final step of ::arraw::encodedForOutput on its own, for an image that was changed in float
/// after it, such as a sharpened one: the same clamp, the same rounding and the same alpha as it
/// gives, so that no value of an output comes from Qt (ADR 057). The colour space is kept.
/// @param floats Straight-alpha `Format_RGBA32FPx4` image in the encoding the result is to have.
/// @param format QImage layout of the result: RGB888, RGBA8888, RGBX64, RGBA64 or RGBA32FPx4.
/// @return The image in @p format.
/// @throws std::invalid_argument if @p floats is not `Format_RGBA32FPx4` or @p format is not one
/// of those above.
/// @throws std::runtime_error if the result cannot be allocated.
[[nodiscard]] QImage quantisedForOutput(const QImage& floats, QImage::Format format);

} // namespace arraw
