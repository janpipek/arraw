#pragma once

#include <Diagnostics.h>
#include <ImageBuffer.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

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

/// @brief Lists the file extensions of the photographs arraw opens, by name.
///
/// The RAW extensions the RAW decoder claims by name, then the standard formats
/// Qt decodes here (`jpg`, `jpeg`, `png`, `tif`, `tiff`), lower-case and without
/// the dot. The one list ::arraw::isSupportedImage and everything that lists a
/// folder go by. A file named otherwise may still decode, since content has a
/// say in ::arraw::loadImage, but it is not offered to a photographer.
/// @return The extensions, valid for the life of the program.
[[nodiscard]] std::span<const std::string_view> supportedImageExtensions();

/// @brief Checks whether a path names a photograph arraw opens.
///
/// By the extension alone, compared case-insensitively; the file is not looked at.
/// @param path Path to inspect.
/// @return `true` if its extension is one of ::arraw::supportedImageExtensions.
[[nodiscard]] bool isSupportedImage(const std::filesystem::path& path);

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

/// @brief How a decode trades completeness for speed.
struct DecodeOptions {
    /// @brief Whether to decode a RAW at half its width and height.
    ///
    /// The RAW decoder then combines each 2x2 block of the sensor into one
    /// pixel instead of demosaicing, which costs a fraction of a full decode
    /// and suits a render that is small anyway (ADR 031). The result is an
    /// ordinary decoded buffer: same encoding, orientation and applied white
    /// balance, with each side `size / 2` (rounded down, as the decoder does),
    /// whatever the file holds (one already demosaiced, a linear DNG, is
    /// halved afterwards, without the speed-up), so ::arraw::develop takes it
    /// unchanged. Its pixel scale is 2 (::arraw::ImageBuffer::pixelScale), so
    /// noise reduction shrinks its reach to match. Its size is then not the one
    /// ::arraw::readImageMetadata declares, so plan against the buffer
    /// (`planFor(buffer, ...)`), not the metadata. Ignored for anything that is
    /// not a RAW: those decode in full, and ::arraw::halved is there to reduce them.
    bool halfSize = false;

    friend bool operator==(const DecodeOptions&, const DecodeOptions&) = default;
};

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
/// @param options How complete the decode must be; see ::arraw::DecodeOptions.
/// @return A buffer holding the decoded pixels.
/// @throws std::runtime_error if the file cannot be opened, decoded, or
/// converted. Very large images are refused by the decoder's own allocation
/// limit rather than being decoded.
[[nodiscard]] ImageBuffer loadImage(const std::filesystem::path& path,
                                    DiagnosticLog& log = discardedDiagnostics(),
                                    DecodeOptions options = {});

/// @brief Reads the preview image a file carries, without decoding the photograph.
///
/// Cameras embed JPEG previews in their RAW files, and JPEG and TIFF files may
/// carry an EXIF thumbnail; reading one costs a fraction of a decode, which is
/// what a thumbnail wants first (ADR 031). Of the previews the file holds, the
/// smallest whose longer edge is at least @p maxEdge is used, else the largest.
/// It is returned as the file's own camera-processed picture and not as arraw
/// would develop it: converted to sRGB, rotated upright by the orientation the
/// file records (a preview that records its own is trusted instead), and
/// reduced, with smooth filtering, to fit @p maxEdge on its longer edge. A
/// preview is never enlarged.
///
/// A file with no preview, such as a PNG, gives no result and no notice. A
/// file that cannot be read at all gives no result and a
/// ::arraw::Notice::PreviewUnreadable warning. May be called from several
/// threads at once.
///
/// @param path File to read.
/// @param maxEdge Longer edge, in pixels, the result must fit and the preview
/// should reach; zero keeps the largest preview at its own size.
/// @param log Where to report a file that could not be read.
/// @return An upright ::arraw::PixelFormat::RgbaU8 buffer in
/// ::arraw::NamedEncoding::Srgb with no pending orientation, or `std::nullopt`.
[[nodiscard]] std::optional<ImageBuffer>
readEmbeddedPreview(const std::filesystem::path& path, std::uint32_t maxEdge,
                    DiagnosticLog& log = discardedDiagnostics());

} // namespace arraw
