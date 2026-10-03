#include "Shot.h"

#include "RawImport.h"

#include <ImageImport.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

using namespace arraw;

namespace {

/// @brief Lower-cases ASCII letters, leaving every other byte, UTF-8 included, alone.
std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

bool isDigit(char character) {
    return std::isdigit(static_cast<unsigned char>(character)) != 0;
}

/// @brief Compares two names in natural order: case-insensitive, runs of digits as numbers.
/// @return Negative, zero or positive as @p left precedes, equals or follows @p right.
int compareNatural(std::string_view left, std::string_view right) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left.size() && j < right.size()) {
        if (isDigit(left[i]) && isDigit(right[j])) {
            std::size_t endLeft = i;
            std::size_t endRight = j;
            while (endLeft < left.size() && isDigit(left[endLeft])) {
                ++endLeft;
            }
            while (endRight < right.size() && isDigit(right[endRight])) {
                ++endRight;
            }
            // Numerically: leading zeros do not count, then more digits is more.
            std::string_view numberLeft = left.substr(i, endLeft - i);
            std::string_view numberRight = right.substr(j, endRight - j);
            numberLeft.remove_prefix(
                std::min(numberLeft.find_first_not_of('0'), numberLeft.size()));
            numberRight.remove_prefix(
                std::min(numberRight.find_first_not_of('0'), numberRight.size()));
            if (numberLeft.size() != numberRight.size()) {
                return numberLeft.size() < numberRight.size() ? -1 : 1;
            }
            if (const int order = numberLeft.compare(numberRight); order != 0) {
                return order;
            }
            i = endLeft;
            j = endRight;
            continue;
        }
        const int a = std::tolower(static_cast<unsigned char>(left[i]));
        const int b = std::tolower(static_cast<unsigned char>(right[j]));
        if (a != b) {
            return a < b ? -1 : 1;
        }
        ++i;
        ++j;
    }
    // The shorter name first; an exhausted side has nothing left to compare.
    return (left.size() - i) == (right.size() - j) ? 0
                                                   : (left.size() - i < right.size() - j ? -1 : 1);
}

/// @brief Orders paths by natural order of the file name, then by folder, then by the exact name.
bool precedes(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::string nameLeft = left.filename().string();
    const std::string nameRight = right.filename().string();
    if (const int order = compareNatural(nameLeft, nameRight); order != 0) {
        return order < 0;
    }
    return std::forward_as_tuple(left.parent_path().native(), nameLeft) <
           std::forward_as_tuple(right.parent_path().native(), nameRight);
}

/// @brief The files of one capture.
struct Bucket {
    std::vector<std::filesystem::path> raws;
    std::vector<std::filesystem::path> standards;
};

/// @brief Canonical name of a file's format: JPEG and TIFF spelled one way, else upper-cased.
std::string formatName(const std::filesystem::path& path) {
    std::string extension = lowered(path.extension().string());
    if (!extension.empty()) {
        extension.erase(0, 1);
    }
    if (extension == "jpg" || extension == "jpeg") {
        return "JPEG";
    }
    if (extension == "tif" || extension == "tiff") {
        return "TIFF";
    }
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return extension;
}

} // namespace

std::vector<Shot> arraw::groupShots(std::vector<std::filesystem::path> paths) {
    // Files of one capture: the same folder and the same stem, in any case.
    std::map<std::pair<std::filesystem::path, std::string>, Bucket> buckets;
    for (auto& path : paths) {
        if (!isSupportedImage(path)) {
            continue;
        }
        Bucket& bucket = buckets[{path.parent_path(), lowered(path.stem().string())}];
        (rawimport::hasRawExtension(path) ? bucket.raws : bucket.standards)
            .push_back(std::move(path));
    }

    std::vector<Shot> shots;
    for (auto& [key, bucket] : buckets) {
        if (bucket.raws.size() == 1) {
            std::ranges::sort(bucket.standards, precedes);
            shots.push_back({std::move(bucket.raws.front()), std::move(bucket.standards)});
            continue;
        }
        // No RAW to be the primary, or two of them and no telling which owns
        // the standard images: every file stands alone.
        for (auto* files : {&bucket.raws, &bucket.standards}) {
            for (auto& path : *files) {
                shots.push_back({std::move(path), {}});
            }
        }
    }
    std::ranges::sort(shots, [](const Shot& left, const Shot& right) {
        return precedes(left.primary, right.primary);
    });
    return shots;
}

std::vector<Shot> arraw::listShots(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    if (error) {
        throw std::runtime_error(folder.string() + ": " + error.message());
    }
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_iterator end; entries != end; entries.increment(error)) {
        if (error) {
            throw std::runtime_error(folder.string() + ": " + error.message());
        }
        const std::filesystem::path& path = entries->path();
        std::error_code ignored;
        if (!path.filename().native().starts_with('.') &&
            std::filesystem::is_regular_file(path, ignored)) {
            files.push_back(path);
        }
    }
    if (error) {
        throw std::runtime_error(folder.string() + ": " + error.message());
    }
    return groupShots(std::move(files));
}

std::string arraw::formatLabel(const Shot& shot) {
    std::vector<std::string> formats;
    const auto add = [&formats](const std::filesystem::path& path) {
        std::string name = formatName(path);
        if (!name.empty() && std::ranges::find(formats, name) == formats.end()) {
            formats.push_back(std::move(name));
        }
    };
    add(shot.primary);
    for (const auto& companion : shot.companions) {
        add(companion);
    }
    std::string label;
    for (const auto& format : formats) {
        label += label.empty() ? "" : "+";
        label += format;
    }
    return label;
}
