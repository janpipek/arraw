#pragma once

#include <Develop.h>
#include <ImageExport.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace arraw {

/// @brief What an export is asked for: the choices behind one export.
///
/// Free of widgets and of files, so that what the choices mean is testable without either.
struct ExportSettings {
    ImageFileFormat format = ImageFileFormat::Jpeg; ///< File format to write.
    NamedEncoding encoding = NamedEncoding::Srgb;   ///< Output colour: sRGB, Display P3, Adobe RGB.
    bool sixteenBit = false;                        ///< Whether to write 16 bits per channel.
    bool resize = false;                            ///< Whether the size below applies.
    std::uint32_t width = 0;                        ///< Widest result, in pixels, when resizing.
    std::uint32_t height = 0;                       ///< Tallest result, in pixels, when resizing.
    bool allowEnlarging = false;                    ///< Whether the size may exceed the frame's.
    int quality = 90;                               ///< JPEG quality, 0-100.
    int sharpening = 0;                             ///< Output sharpening amount, 0-100.
    bool captureInfo = true;                        ///< Whether to carry camera and capture info.
    bool location = false;                          ///< Whether to carry the GPS location.
    bool descriptive = true; ///< Whether to carry rating, label, title, rights.

    /// @brief Compares every field.
    [[nodiscard]] bool operator==(const ExportSettings&) const = default;
};

/// @brief Drops what the format and size do not use.
///
/// Width, height and enlarging are 0/false when not resizing, and 16 bits is off for JPEG, which
/// has only 8. Two settings that export alike have one canonical form.
/// @param settings What the user chose.
[[nodiscard]] ExportSettings canonical(ExportSettings settings);

/// @brief Checks that settings can be exported.
///
/// Quality and sharpening are 0 to 100; the encoding is an output one; a box, when resizing, is 1
/// to 99999 on each side.
/// @param settings What the user chose.
/// @throws std::invalid_argument naming the field that is out of range.
void validate(const ExportSettings& settings);

/// @brief Makes the render request for settings.
/// @param settings What the user chose.
/// @return A request for the whole frame: fitting the box when resizing, else at its own size.
[[nodiscard]] RenderRequest requestOf(const ExportSettings& settings);

/// @brief Makes the export options for settings.
///
/// 16 bits per channel is ignored for JPEG, which has only 8.
/// @param settings What the user chose.
[[nodiscard]] ExportOptions optionsOf(const ExportSettings& settings);

/// @brief Maps the metadata choices of settings to the groups an export carries.
/// @param settings What the user chose.
[[nodiscard]] MetadataSelection selectionOf(const ExportSettings& settings);

/// @brief Lists the long edges, in pixels, the export dialog offers besides the original size.
[[nodiscard]] std::span<const std::uint32_t> presetLongEdges();

/// @brief Gives the stable word for a file format.
/// @return "jpeg", "png" or "tiff".
[[nodiscard]] std::string_view tokenOf(ImageFileFormat format);

/// @brief Gives the stable word for an output encoding.
/// @return "srgb", "display-p3" or "adobe-rgb".
/// @throws std::invalid_argument for an encoding that is no output one.
[[nodiscard]] std::string_view tokenOf(NamedEncoding encoding);

/// @brief Reads a file format from its word.
///
/// Lower case only; the canonical tokens and the aliases "jpg" and "tif".
/// @return The format, or nothing for an unknown word.
[[nodiscard]] std::optional<ImageFileFormat> parseImageFileFormat(std::string_view word);

/// @brief Reads an output encoding from its word.
///
/// Lower case only; the canonical tokens.
/// @return The encoding, or nothing for an unknown word.
[[nodiscard]] std::optional<NamedEncoding> parseOutputEncoding(std::string_view word);

} // namespace arraw
