#pragma once

#include <filesystem>

#include <Diagnostics.h>
#include <ImageBuffer.h>
#include <ImageImport.h>

/// @brief RAW decoding, over LibRaw.
///
/// Everything that knows how a sensor file becomes an ::arraw::ImageBuffer
/// lives here, so ::arraw::loadImage need only decide which decoder a file
/// belongs to. Replacing LibRaw one day means replacing this file.
namespace arraw::rawimport {

/// @brief Checks whether a path's extension names a RAW format arraw decodes.
///
/// A fast path, not the decision: ::holdsRawImage asks the content of
/// everything this declines, and asks it before Qt's codecs are offered
/// anything. Naming the extension saves that second open in the common case,
/// and gives a file named like a RAW the RAW decoder's error message when it
/// turns out not to be one.
/// @param path Path to inspect; its extension is compared case-insensitively.
/// @return `true` for a recognised RAW extension.
[[nodiscard]] bool namesRawFormat(const std::filesystem::path& path);

/// @brief Checks whether a file's content is a RAW image, whatever it is named.
///
/// Parses the file's headers without unpacking any pixels, so it is cheap
/// enough to ask of every file — which ::arraw::loadImage does, before it
/// offers anything to Qt. Asking afterwards is too late: a RAW container is
/// usually a TIFF carrying an ordinary RGB preview, and a TIFF reader decodes
/// that preview rather than failing, so a fallback for "Qt could not read it"
/// never runs.
///
/// Safe to ask first because LibRaw claims camera files, not TIFFs: measured
/// against 0.22.2, every multi-channel TIFF offered to it is declined,
/// arraw's own exports included.
/// @param path File to inspect.
/// @return `true` if LibRaw recognises the content.
[[nodiscard]] bool holdsRawImage(const std::filesystem::path& path);

/// @brief Reads what a RAW file declares, without unpacking its pixels.
///
/// LibRaw fills its colour description from the file's headers, so the camera
/// matrix, the daylight scale and the as-shot neutral are all available for
/// the cost of an open — which is what lets a photograph be described, and a
/// render planned, without a demosaic (ADR 012).
///
/// The dimensions are the visible frame's, before any orientation: ::load
/// applies none, so they are the ones it will produce.
///
/// @param path File to describe.
/// @param log Where to report a substituted white balance.
/// @return The file's dimensions and its camera's colour.
/// @throws std::runtime_error if the file cannot be opened or read.
[[nodiscard]] ImageMetadata readMetadata(const std::filesystem::path& path, DiagnosticLog& log);

/// @brief Decodes a RAW file into a buffer in the working encoding.
///
/// The result is a *neutral development*, not sensor data: LibRaw demosaics,
/// applies the camera's as-shot white balance — or, for a file that declares
/// none, the daylight multipliers its colour matrix implies — and converts
/// through that matrix into linear Rec.2020. White balance, demosaic and
/// highlight handling are develop settings that this bakes in; see ADR 005 for
/// why, and for what a later `RawLoadOptions` would reopen.
///
/// Orientation is retained on the buffer without rearranging pixels, matching
/// ::arraw::loadImage. Development applies it before the user's geometry.
///
/// @param path File to decode.
/// @param log Where to report a substituted white balance.
/// @return A buffer holding the decoded pixels, RGBA at sixteen bits per
/// channel, opaque.
/// @throws std::runtime_error if the file cannot be opened, decoded, or
/// converted.
[[nodiscard]] ImageBuffer load(const std::filesystem::path& path, DiagnosticLog& log);

} // namespace arraw::rawimport
