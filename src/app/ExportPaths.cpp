#include "ExportPaths.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <string>

namespace arraw::app {

namespace {

/// @brief Lower-cases ASCII, which is all an extension needs.
std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

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

std::vector<std::filesystem::path> targetPaths(std::span<const std::filesystem::path> sources,
                                               const std::filesystem::path& folder,
                                               ImageFileFormat format) {
    // Names compare in lower case on every platform: a folder may be case-insensitive.
    const auto key = [](const std::filesystem::path& path) {
        return lowered(path.generic_string());
    };
    std::set<std::string> taken;
    for (const auto& source : sources) {
        taken.insert(key(source));
    }
    std::vector<std::filesystem::path> targets;
    targets.reserve(sources.size());
    for (const auto& source : sources) {
        const std::string stem = source.stem().string();
        std::filesystem::path candidate;
        for (int n = 1;; ++n) {
            const std::string name = (n == 1 ? stem : stem + "_" + std::to_string(n)) + "." +
                                     std::string(suffixOf(format));
            candidate = folder / name;
            if (taken.insert(key(candidate)).second) {
                break;
            }
        }
        targets.push_back(std::move(candidate));
    }
    return targets;
}

std::filesystem::path suggestedPath(const std::filesystem::path& source, ImageFileFormat format) {
    std::filesystem::path name = source.stem();
    name += "." + std::string(suffixOf(format));
    return source.parent_path() / name;
}

} // namespace arraw::app
