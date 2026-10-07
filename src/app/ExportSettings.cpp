#include "ExportSettings.h"

#include <QString>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace arraw::app {

namespace {

/// Group of the stored keys.
const QString group = QStringLiteral("export/");

/// @brief Lower-cases ASCII, which is all an extension needs.
std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// @brief Reads a stored integer within a range, or a fallback.
int storedInt(QSettings& store, const char* key, int fallback, int low, int high) {
    bool ok = false;
    const int value = store.value(group + QLatin1String(key), fallback).toInt(&ok);
    return ok && value >= low && value <= high ? value : fallback;
}

} // namespace

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

std::string_view suffixOf(ImageFileFormat format) {
    switch (format) {
    case ImageFileFormat::Jpeg:
        return "jpg";
    case ImageFileFormat::Png:
        return "png";
    case ImageFileFormat::Tiff:
        return "tif";
    }
    return "jpg";
}

std::filesystem::path withSuffix(const std::filesystem::path& path, ImageFileFormat format) {
    constexpr std::array known{".jpg", ".jpeg", ".png", ".tif", ".tiff"};
    const std::string current = lowered(path.extension().string());
    if (std::ranges::find(known, current) == known.end()) {
        return path.string() + "." + std::string(suffixOf(format));
    }
    const bool own =
        (format == ImageFileFormat::Jpeg && (current == ".jpg" || current == ".jpeg")) ||
        (format == ImageFileFormat::Png && current == ".png") ||
        (format == ImageFileFormat::Tiff && (current == ".tif" || current == ".tiff"));
    if (own) {
        return path;
    }
    std::filesystem::path result = path;
    return result.replace_extension("." + std::string(suffixOf(format)));
}

std::filesystem::path suggestedPath(const std::filesystem::path& source, ImageFileFormat format) {
    std::filesystem::path name = source.stem();
    name += "." + std::string(suffixOf(format));
    return source.parent_path() / name;
}

void saveSettings(const ExportSettings& settings, QSettings& store) {
    store.setValue(group + "format", static_cast<int>(settings.format));
    store.setValue(group + "encoding", static_cast<int>(settings.encoding));
    store.setValue(group + "sixteenBit", settings.sixteenBit);
    store.setValue(group + "resize", settings.resize);
    store.setValue(group + "allowEnlarging", settings.allowEnlarging);
    store.setValue(group + "quality", settings.quality);
    store.setValue(group + "sharpening", settings.sharpening);
    store.setValue(group + "captureInfo", settings.captureInfo);
    store.setValue(group + "location", settings.location);
    store.setValue(group + "descriptive", settings.descriptive);
}

ExportSettings restoreSettings(QSettings& store) {
    ExportSettings settings;
    switch (storedInt(store, "format", static_cast<int>(settings.format), 0, 2)) {
    case static_cast<int>(ImageFileFormat::Png):
        settings.format = ImageFileFormat::Png;
        break;
    case static_cast<int>(ImageFileFormat::Tiff):
        settings.format = ImageFileFormat::Tiff;
        break;
    default:
        break;
    }
    const int encoding =
        storedInt(store, "encoding", static_cast<int>(settings.encoding),
                  static_cast<int>(NamedEncoding::Srgb), static_cast<int>(NamedEncoding::AdobeRgb));
    settings.encoding = static_cast<NamedEncoding>(encoding);
    settings.sixteenBit = store.value(group + "sixteenBit", settings.sixteenBit).toBool();
    settings.resize = store.value(group + "resize", settings.resize).toBool();
    settings.allowEnlarging =
        store.value(group + "allowEnlarging", settings.allowEnlarging).toBool();
    settings.quality = storedInt(store, "quality", settings.quality, 0, 100);
    settings.sharpening = storedInt(store, "sharpening", settings.sharpening, 0, 100);
    settings.captureInfo = store.value(group + "captureInfo", settings.captureInfo).toBool();
    settings.location = store.value(group + "location", settings.location).toBool();
    settings.descriptive = store.value(group + "descriptive", settings.descriptive).toBool();
    return settings;
}

} // namespace arraw::app
