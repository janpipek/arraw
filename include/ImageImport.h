#pragma once

#include <filesystem>

#include <ImageBuffer.h>

namespace arraw {
/// @brief Decodes an image file into a buffer in the working encoding.
///
/// The format is detected from the file's content rather than its extension,
/// so a misnamed file still loads. An embedded ICC profile is honoured; a file
/// without one is taken to be sRGB. Samples are converted into
/// ::arraw::workingEncoding, and the result is always RGBA at sixteen bits per
/// channel or wider, because eight linear bits cannot carry the shadows.
///
/// Orientation metadata is not applied: the pixels are returned as stored, and
/// rotation remains a develop setting rather than something baked into the
/// buffer.
///
/// @param path File to decode.
/// @return A buffer holding the decoded pixels.
/// @throws std::runtime_error if the file cannot be opened, decoded, or
/// converted. Very large images are refused by the decoder's own allocation
/// limit rather than being decoded.
[[nodiscard]] ImageBuffer loadImage(const std::filesystem::path& path);

} // namespace arraw
