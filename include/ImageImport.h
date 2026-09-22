#pragma once

#include <filesystem>

#include <Diagnostics.h>
#include <ImageBuffer.h>

namespace arraw {

/// @brief What a photograph declares about itself, read without decoding it.
///
/// What a plan is resolved from (ADR 012): a photograph can be described
/// before any pixel is demosaiced, so opening a document does not cost a
/// decode, and the description a render is planned against is the one the
/// decode will honour.
struct ImageMetadata {
    /// @brief Pixel dimensions the decode will produce.
    ImageSize size;

    /// @brief Encoding the decoded samples will be in.
    ///
    /// A camera's own space for a RAW, because the conversion out of it
    /// belongs to development (ADR 007); the working encoding for anything
    /// else, which ::arraw::loadImage converts to on the way in.
    ColorEncoding encoding;

    /// @brief Camera orientation, retained without rearranging decoded pixels.
    ImageOrientation orientation = ImageOrientation::Normal;

    friend bool operator==(const ImageMetadata&, const ImageMetadata&) = default;
};

/// @brief Reads what a file declares about itself, without decoding its pixels.
///
/// The same decoder decides as in ::arraw::loadImage, by the same rule, so a
/// photograph is described by whatever will decode it. For a RAW this parses
/// the file's headers; for anything else it reads the image header Qt's codec
/// exposes. Neither unpacks a pixel.
///
/// @param path File to describe.
/// @param log Where to report what a photographer should know about the file,
/// such as a white balance it did not record.
/// @return What the file declares.
/// @throws std::runtime_error if the file cannot be opened or is not an image
/// either decoder recognises.
[[nodiscard]] ImageMetadata readImageMetadata(const std::filesystem::path& path,
                                              DiagnosticLog& log = discardedDiagnostics());

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
/// Orientation metadata is retained on the buffer without rearranging pixels,
/// for RAW files as for any other. Development applies that camera orientation
/// before the user's geometry settings.
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
