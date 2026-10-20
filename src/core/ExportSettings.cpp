#include <ExportSettings.h>

#include <array>
#include <stdexcept>
#include <string>

namespace arraw {

namespace {

/// Long edges the export dialog offers.
constexpr std::array<std::uint32_t, 4> longEdges{1080, 1600, 2048, 3840};

/// Greatest side of an export box, in pixels.
constexpr std::uint32_t maxSide = 99999;

/// @brief Throws unless value lies within [low, high].
void requireRange(const char* field, long long value, long long low, long long high) {
    if (value < low || value > high) {
        throw std::invalid_argument(std::string(field) + " must be from " + std::to_string(low) +
                                    " to " + std::to_string(high));
    }
}

} // namespace

std::span<const std::uint32_t> presetLongEdges() {
    return longEdges;
}

ExportSettings canonical(ExportSettings settings) {
    if (!settings.resize) {
        settings.width = 0;
        settings.height = 0;
        settings.allowEnlarging = false;
    }
    if (settings.format == ImageFileFormat::Jpeg) {
        settings.sixteenBit = false;
    }
    return settings;
}

void validate(const ExportSettings& settings) {
    requireRange("quality", settings.quality, 0, 100);
    requireRange("sharpening", settings.sharpening, 0, 100);
    (void)tokenOf(settings.encoding);
    if (settings.resize) {
        requireRange("width", settings.width, 1, maxSide);
        requireRange("height", settings.height, 1, maxSide);
    }
}

RenderRequest requestOf(const ExportSettings& settings) {
    RenderRequest request;
    if (settings.resize) {
        request.size = RenderRequest::FitInside{settings.width, settings.height};
        request.upscale = settings.allowEnlarging ? Upscale::Allowed : Upscale::Never;
    }
    return request;
}

ExportOptions optionsOf(const ExportSettings& settings) {
    ExportOptions options;
    options.format = settings.format;
    options.encoding = settings.encoding;
    options.bitDepth = settings.sixteenBit && settings.format != ImageFileFormat::Jpeg ? 16 : 8;
    options.quality = settings.quality;
    options.sharpening = settings.sharpening;
    return options;
}

MetadataSelection selectionOf(const ExportSettings& settings) {
    return {.capture = settings.captureInfo,
            .location = settings.location,
            .descriptive = settings.descriptive};
}

std::string_view tokenOf(ImageFileFormat format) {
    switch (format) {
    case ImageFileFormat::Jpeg:
        return "jpeg";
    case ImageFileFormat::Png:
        return "png";
    case ImageFileFormat::Tiff:
        return "tiff";
    }
    throw std::invalid_argument("unknown image file format");
}

std::string_view tokenOf(NamedEncoding encoding) {
    switch (encoding) {
    case NamedEncoding::Srgb:
        return "srgb";
    case NamedEncoding::DisplayP3:
        return "display-p3";
    case NamedEncoding::AdobeRgb:
        return "adobe-rgb";
    default:
        break;
    }
    throw std::invalid_argument("the encoding is not an output encoding");
}

std::optional<ImageFileFormat> parseImageFileFormat(std::string_view word) {
    if (word == "jpeg" || word == "jpg") {
        return ImageFileFormat::Jpeg;
    }
    if (word == "png") {
        return ImageFileFormat::Png;
    }
    if (word == "tiff" || word == "tif") {
        return ImageFileFormat::Tiff;
    }
    return std::nullopt;
}

std::optional<NamedEncoding> parseOutputEncoding(std::string_view word) {
    if (word == "srgb") {
        return NamedEncoding::Srgb;
    }
    if (word == "display-p3") {
        return NamedEncoding::DisplayP3;
    }
    if (word == "adobe-rgb") {
        return NamedEncoding::AdobeRgb;
    }
    return std::nullopt;
}

} // namespace arraw
