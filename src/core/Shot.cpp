#include "Shot.h"

#include "RawImport.h"

#include <ImageImport.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <map>
#include <ranges>
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

/// @brief The UTF-8 bytes of a path component, whatever the platform's narrow code page.
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
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
    const std::string nameLeft = utf8(left.filename());
    const std::string nameRight = utf8(right.filename());
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
    std::string extension = lowered(utf8(path.extension()));
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
        Bucket& bucket = buckets[{path.parent_path(), lowered(utf8(path.stem()))}];
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

namespace {

/// @brief Tells whether a folder name is one the recursive listing enters: not hidden, not clutter.
bool isListedFolderName(const std::filesystem::path& name) {
    const std::string text = utf8(name);
    return !text.starts_with('.') && text != "@eaDir" && text != "#recycle" &&
           text != "$RECYCLE.BIN" && text != "System Volume Information";
}

/// @brief Compares two path components: natural order, then the exact bytes.
int compareComponents(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::string a = utf8(left);
    const std::string b = utf8(right);
    if (const int order = compareNatural(a, b); order != 0) {
        return order;
    }
    return a.compare(b);
}

/// @brief The error of a folder that cannot be read; its text is `<folder>: <reason>`, as the
/// application's dialog and the command line show it.
class UnreadableFolder : public std::filesystem::filesystem_error {
public:
    UnreadableFolder(const std::filesystem::path& folder, std::error_code error)
        : filesystem_error("cannot read the folder", folder, error),
          text_(folder.string() + ": " + error.message()) {}

    [[nodiscard]] const char* what() const noexcept override {
        return text_.c_str();
    }

private:
    std::string text_;
};

/// @brief Throws the error of a folder that cannot be read, naming it.
[[noreturn]] void throwUnreadable(const std::filesystem::path& folder, std::error_code error) {
    throw UnreadableFolder(folder, error);
}

} // namespace

bool arraw::isListedFolder(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    return entry.symlink_status(error).type() == std::filesystem::file_type::directory &&
           isListedFolderName(entry.path().filename());
}

bool arraw::folderPrecedes(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto left = a.begin();
    auto right = b.begin();
    for (; left != a.end() && right != b.end(); ++left, ++right) {
        if (const int order = compareComponents(*left, *right); order != 0) {
            return order < 0;
        }
    }
    // One is a prefix of the other: the folder comes before its subtree.
    return left == a.end() && right != b.end();
}

FolderListing arraw::listFolder(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    if (error) {
        throwUnreadable(folder, error);
    }
    FolderListing listing;
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_iterator end; entries != end; entries.increment(error)) {
        const std::filesystem::directory_entry& entry = *entries;
        // The type the directory read reported (d_type, or the find data on Windows); one
        // lstat only where the filesystem reports none.
        std::error_code ignored;
        const std::filesystem::file_type type = entry.symlink_status(ignored).type();
        const std::filesystem::path name = entry.path().filename();
        if (type == std::filesystem::file_type::directory) {
            if (isListedFolderName(name)) {
                listing.folders.push_back(entry.path());
            }
            continue;
        }
        if (name.native().starts_with('.')) {
            continue;
        }
        // A link is followed, so a link to a photograph lists; that costs the one stat.
        if (type == std::filesystem::file_type::regular ||
            (type == std::filesystem::file_type::symlink && entry.is_regular_file(ignored))) {
            files.push_back(entry.path());
        }
    }
    // A failed increment leaves the iterator at its end, so the loop has stopped on it.
    if (error) {
        throwUnreadable(folder, error);
    }
    std::ranges::sort(listing.folders, folderPrecedes);
    listing.shots = groupShots(std::move(files));
    return listing;
}

FolderError arraw::classifyFolderError(const std::error_code& error) {
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        return FolderError::Gone;
    }
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted) {
        return FolderError::NoPermission;
    }
    return FolderError::Other;
}

std::vector<Shot> arraw::listShots(const std::filesystem::path& folder, bool recursive) {
    FolderListing root = listFolder(folder);
    if (!recursive) {
        return std::move(root.shots);
    }
    // Pre-order, children in folderPrecedes order: the stack holds them reversed, so the
    // first child is read next. Only the root may throw; a subfolder that cannot be read
    // (permission denied, vanished, a dead mount) is skipped and its siblings still listed.
    std::vector<Shot> shots = std::move(root.shots);
    std::vector<std::filesystem::path> pending(std::make_move_iterator(root.folders.rbegin()),
                                               std::make_move_iterator(root.folders.rend()));
    while (!pending.empty()) {
        const std::filesystem::path current = std::move(pending.back());
        pending.pop_back();
        FolderListing listing;
        try {
            listing = listFolder(current);
        } catch (const std::filesystem::filesystem_error&) {
            continue;
        }
        std::ranges::move(listing.shots, std::back_inserter(shots));
        std::ranges::move(listing.folders | std::views::reverse, std::back_inserter(pending));
    }
    return shots;
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
