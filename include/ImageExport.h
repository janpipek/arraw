#pragma once

#include <Diagnostics.h>
#include <ImageBuffer.h>
#include <PhotoMarks.h>

#include <filesystem>
#include <optional>

namespace arraw {
/// @brief Supported image file formats.
enum class ImageFileFormat { Jpeg, Png, Tiff };

/// @brief Output colour, precision, and compression settings.
struct ExportOptions {
    std::optional<ImageFileFormat> format =
        std::nullopt;                             ///< Derived from the extension when absent.
    NamedEncoding encoding = NamedEncoding::Srgb; ///< sRGB, Display P3, or Adobe RGB.
    int bitDepth = 8;                             ///< Bits per channel: 8 or 16; JPEG requires 8.
    int quality = 90;                             ///< JPEG quality, 0–100; ignored for PNG/TIFF.
    bool embedProfile = true; ///< Embedded output ICC profile; conversion always applies.
    int sharpening = 0;       ///< Output sharpening amount, 0–100; 0 is off (ADR 026).
};

/// @brief Groups of metadata an export can carry from its source (ADR 032).
struct MetadataSelection {
    /// @brief Camera, lens, exposure and capture time; never serial numbers or maker notes.
    bool capture = true;

    /// @brief Where the photograph was taken (the GPS tags); off by default as it can reveal a
    /// home.
    bool location = false;

    /// @brief Rating, label, title, caption, keywords, creator and rights.
    bool descriptive = true;

    friend bool operator==(const MetadataSelection&, const MetadataSelection&) = default;
};

/// @brief Where an export takes its metadata from, and which groups it takes.
struct ExportMetadata {
    /// @brief Photograph the pixels came from; its EXIF, XMP and sidecar are read.
    std::filesystem::path source;

    /// @brief Culling marks to write as `xmp:Rating` and `xmp:Label`.
    PhotoMarks marks;

    /// @brief Groups to carry.
    MetadataSelection selection;

    /// @brief Whether the descriptive group also reads the source's sidecar, which wins per
    /// property; false for a caller that ignores sidecars.
    bool useSidecar = true;
};

/// @brief Converts and atomically writes an image to the destination.
/// @param image Source in RgbU8, RgbaU8, RgbaU16, or RgbaF32 layout, encoded
/// as sRGB, Display P3, or Adobe RGB. JPEG requires fully opaque pixels.
/// @param path Destination to create or replace after successful encoding.
/// @param options Output settings; the working encoding is not a valid output, and
/// sharpening must be between 0 and 100.
/// @param metadata What to carry over from the source photograph. Absent, or with
/// no group selected, the file is exactly what it would be without metadata. Otherwise
/// the chosen groups are copied with exiv2 into the encoded file before it replaces
/// the destination (never MakerNotes, thumbnails or the source's pixel-describing
/// tags), and Orientation, pixel dimensions, ColorSpace and Software are set for the
/// output. A source with nothing to carry writes nothing. Reading what to carry is
/// best effort: a source exiv2 cannot read, a sidecar that is not XMP or a tag that cannot
/// be copied is left out with a warning, and the export succeeds.
/// @param log Receives ::arraw::Notice::MetadataNotCarried warnings, naming the source.
/// @throws std::invalid_argument if the input, format, or options are unsupported, or
/// the rating of @p metadata is outside -1 to 5.
/// @throws std::runtime_error if image preparation, encoding, or file writing fails,
/// or the metadata cannot be written into the encoded file, naming the cause; the
/// destination is then untouched.
void exportImage(const ImageBuffer& image, const std::filesystem::path& path,
                 const ExportOptions& options,
                 const std::optional<ExportMetadata>& metadata = std::nullopt,
                 DiagnosticLog& log = discardedDiagnostics());

/// @brief Checks whether two paths name the same file, which may be one that does not exist yet.
///
/// Follows links when both exist; otherwise compares the paths made absolute
/// and normalised. Used to keep an export from replacing its own input.
/// @param a First path.
/// @param b Second path.
/// @return True if both name one file.
[[nodiscard]] bool isSameFile(const std::filesystem::path& a, const std::filesystem::path& b);
} // namespace arraw
