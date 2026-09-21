#pragma once

#include <filesystem>
#include <optional>

#include <ImageBuffer.h>

namespace arraw {
/// @brief Supported image file formats.
enum class ImageFileFormat { Jpeg, Png, Tiff };

/// @brief Output colour, precision, and compression settings.
struct ExportOptions {
    std::optional<ImageFileFormat> format =
        std::nullopt;                             ///< Derived from the extension when absent.
    ColorEncoding encoding = ColorEncoding::Srgb; ///< sRGB, Display P3, or Adobe RGB.
    int bitDepth = 8;                             ///< Bits per channel: 8 or 16; JPEG requires 8.
    int quality = 90;                             ///< JPEG quality, 0–100; ignored for PNG/TIFF.
    bool embedProfile = true; ///< Embedded output ICC profile; conversion always applies.
};

/// @brief Converts and atomically writes an image to the destination.
/// @param image Source in RgbU8, RgbaU8, RgbaU16, or RgbaF32 layout, encoded
/// as sRGB, Display P3, or Adobe RGB. JPEG requires fully opaque pixels.
/// @param path Destination to create or replace after successful encoding.
/// @param options Output settings; the working encoding is not a valid output.
/// @throws std::invalid_argument if the input, format, or options are unsupported.
/// @throws std::runtime_error if image preparation, encoding, or file writing fails.
void exportImage(const ImageBuffer& image, const std::filesystem::path& path,
                 const ExportOptions& options);
} // namespace arraw
