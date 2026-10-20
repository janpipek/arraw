#pragma once

#include <ColorEncoding.h>
#include <ImageBuffer.h>

#include <QImage>

namespace arraw {

/// @brief Converts a buffer's colours to an output encoding, in the layout asked for.
///
/// The conversion an export makes before it writes, shared with the output histogram so that the
/// two cannot disagree. The image carries the target's colour space.
/// @param image Buffer in a named encoding, in a layout QImage can wrap.
/// @param encoding Target encoding.
/// @param format QImage layout of the result.
/// @return The converted image.
/// @throws std::invalid_argument if @p image is camera-native or has no QImage layout.
/// @throws std::runtime_error if Qt cannot convert it.
[[nodiscard]] QImage encodedForOutput(const ImageBuffer& image, NamedEncoding encoding,
                                      QImage::Format format);

} // namespace arraw
