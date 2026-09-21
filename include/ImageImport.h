#pragma once

#include <filesystem>

#include <Diagnostics.h>
#include <ImageBuffer.h>

namespace arraw {
/// @brief Decodes an image file into a buffer in the working encoding.
///
/// A RAW extension chooses the RAW decoder, and so does a file whose *content*
/// is a RAW, whatever it happens to be named — both before the general-purpose
/// codecs are offered anything, because a RAW file usually carries a small
/// preview image that those codecs would cheerfully decode in place of the
/// photograph. Anything else is detected from its content as usual. A misnamed
/// file still loads, only more slowly than a correctly named one. Extension
/// decides which decoder, never which format a decoder then reads.
///
/// An embedded ICC profile is honoured; a file without one is taken to be sRGB.
/// Samples are converted into ::arraw::workingEncoding, and the result is
/// always RGBA at sixteen bits per channel or wider, because eight linear bits
/// cannot carry the shadows.
///
/// A RAW file arrives as a *neutral development* rather than sensor data: it is
/// demosaiced, carries the camera's as-shot white balance (or, if it declares
/// none, a fixed daylight one rather than a guess from the frame), and has been
/// through the camera's colour matrix. White balance, demosaic and highlight
/// handling are develop settings that this bakes in; ADR 005 records why, and
/// what a later `RawLoadOptions` would reopen.
///
/// Orientation metadata is not applied, for RAW files as for any other: the
/// pixels are returned as stored, and rotation remains a develop setting rather
/// than something baked into the buffer.
///
/// @param path File to decode.
/// @param log Where to report what a photographer should know about the
/// decode, such as a white balance the file did not record.
/// @return A buffer holding the decoded pixels.
/// @throws std::runtime_error if the file cannot be opened, decoded, or
/// converted. Very large images are refused by the decoder's own allocation
/// limit rather than being decoded.
[[nodiscard]] ImageBuffer loadImage(const std::filesystem::path& path,
                                    DiagnosticLog& log = discardedDiagnostics());

} // namespace arraw
