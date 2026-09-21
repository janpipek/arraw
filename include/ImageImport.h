#pragma once

#include <filesystem>

#include <ImageBuffer.h>

namespace arraw {
/// @brief Decodes an image file into a buffer in the working encoding.
///
/// A RAW extension chooses the RAW decoder; anything else is detected from the
/// file's content, and a file that content detection cannot read is offered to
/// the RAW decoder as a last resort. So a misnamed file still loads, only more
/// slowly than a correctly named one. Extension decides which decoder, never
/// which format a decoder then reads.
///
/// An embedded ICC profile is honoured; a file without one is taken to be sRGB.
/// Samples are converted into ::arraw::workingEncoding, and the result is
/// always RGBA at sixteen bits per channel or wider, because eight linear bits
/// cannot carry the shadows.
///
/// A RAW file arrives as a *neutral development* rather than sensor data: it is
/// demosaiced, carries the camera's as-shot white balance, and has been through
/// the camera's colour matrix. White balance and demosaic are develop settings
/// that this bakes in; ADR 005 records why, and what a later `RawLoadOptions`
/// would reopen.
///
/// Orientation metadata is not applied, for RAW files as for any other: the
/// pixels are returned as stored, and rotation remains a develop setting rather
/// than something baked into the buffer.
///
/// @param path File to decode.
/// @return A buffer holding the decoded pixels.
/// @throws std::runtime_error if the file cannot be opened, decoded, or
/// converted. Very large images are refused by the decoder's own allocation
/// limit rather than being decoded.
[[nodiscard]] ImageBuffer loadImage(const std::filesystem::path& path);

} // namespace arraw
