#pragma once

#include <filesystem>

#include <ImageBuffer.h>

/// @brief RAW decoding, over LibRaw.
///
/// Everything that knows how a sensor file becomes an ::arraw::ImageBuffer
/// lives here, so ::arraw::loadImage need only decide which decoder a file
/// belongs to. Replacing LibRaw one day means replacing this file.
namespace arraw::rawimport {

/// @brief Checks whether a path's extension names a RAW format arraw decodes.
///
/// Extension rather than content, because LibRaw opens ordinary TIFFs as
/// happily as it opens camera files; asking it first would capture arraw's own
/// TIFF exports. Content is consulted only when this returns `false` and Qt's
/// codecs have already declined; see ::holdsRawImage.
/// @param path Path to inspect; its extension is compared case-insensitively.
/// @return `true` for a recognised RAW extension.
[[nodiscard]] bool namesRawFormat(const std::filesystem::path& path);

/// @brief Checks whether a file's content is a RAW image, whatever it is named.
///
/// Parses the file's headers without unpacking any pixels, so it is cheap
/// enough to use as a fallback for a file Qt could not read.
/// @param path File to inspect.
/// @return `true` if LibRaw recognises the content.
[[nodiscard]] bool holdsRawImage(const std::filesystem::path& path);

/// @brief Decodes a RAW file into a buffer in the working encoding.
///
/// The result is a *neutral development*, not sensor data: LibRaw demosaics,
/// applies the camera's as-shot white balance, and converts through the
/// camera's colour matrix into linear Rec.2020. White balance and demosaic are
/// develop settings that this bakes in; see ADR 005 for why, and for what a
/// later `RawLoadOptions` would reopen.
///
/// Orientation is not applied, matching ::arraw::loadImage: rotation stays a
/// develop setting rather than something baked into the buffer.
///
/// @param path File to decode.
/// @return A buffer holding the decoded pixels, RGBA at sixteen bits per
/// channel, opaque.
/// @throws std::runtime_error if the file cannot be opened, decoded, or
/// converted.
[[nodiscard]] ImageBuffer load(const std::filesystem::path& path);

} // namespace arraw::rawimport
