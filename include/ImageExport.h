#pragma once

#include <optional>
#include <filesystem>

#include <ImageBuffer.h>

namespace arraw {
    enum class ImageFileFormat { Jpeg, Png, Tiff };

    struct ExportOptions {
        std::optional<ImageFileFormat> format;      ///< Derived from the extension when absent.
        ColorEncoding encoding = ColorEncoding::Srgb;
        int bitDepth = 8;                       ///< 8 or 16; PNG and TIFF only.
        int quality = 90;                       ///< JPEG only, 0–100.
        bool embedProfile = true;
    };

    void exportImage(const ImageBuffer& image, const std::filesystem::path& path, const ExportOptions& options);
}