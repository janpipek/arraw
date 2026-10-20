#pragma once

#include <Shot.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

/// @file
/// The recursive ::arraw::listShots as it was before it became a walk over ::arraw::listFolder
/// (incremental-refresh plan, step 1): one recursive walk collecting files, a map by folder and
/// a sort of the folders. Kept for the equivalence test and as the benchmark's baseline.

namespace arraw::test::reference {

/// @brief Compares two names in natural order: case-insensitive, runs of digits as numbers.
inline int compareNatural(std::string_view left, std::string_view right) {
    const auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
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
    return (left.size() - i) == (right.size() - j) ? 0
                                                   : (left.size() - i < right.size() - j ? -1 : 1);
}

/// @brief Returns the UTF-8 bytes of a path.
inline std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

/// @brief Compares two folders component by component, so that a folder's subtree follows it.
inline int compareFolders(const std::filesystem::path& left, const std::filesystem::path& right) {
    auto a = left.begin();
    auto b = right.begin();
    for (; a != left.end() && b != right.end(); ++a, ++b) {
        if (const int order = compareNatural(utf8(*a), utf8(*b)); order != 0) {
            return order;
        }
    }
    if (a == left.end() && b == right.end()) {
        return 0;
    }
    return a == left.end() ? -1 : 1;
}

/// @brief Lists the regular files of a folder tree; only the root can make it throw.
inline std::vector<std::filesystem::path> filesUnder(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> pending{root};
    while (!pending.empty()) {
        const std::filesystem::path folder = std::move(pending.back());
        pending.pop_back();
        std::error_code error;
        std::filesystem::directory_iterator entries(folder, error);
        if (error) {
            if (folder == root) {
                throw std::runtime_error(folder.string() + ": " + error.message());
            }
            continue;
        }
        for (const std::filesystem::directory_iterator end; entries != end;
             entries.increment(error)) {
            if (error) {
                break;
            }
            const std::filesystem::path& path = entries->path();
            if (isListedFolder(*entries)) {
                pending.push_back(path);
                continue;
            }
            std::error_code ignored;
            if (!path.filename().native().starts_with('.') &&
                std::filesystem::is_regular_file(path, ignored)) {
                files.push_back(path);
            }
        }
    }
    return files;
}

/// @brief Lists the shots of a folder tree, as the recursive ::arraw::listShots did.
inline std::vector<Shot> listShotsRecursively(const std::filesystem::path& folder) {
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> byFolder;
    for (auto& file : filesUnder(folder)) {
        byFolder[file.parent_path()].push_back(std::move(file));
    }
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : byFolder) {
        folders.push_back(entry.first);
    }
    std::ranges::sort(folders, [](const auto& left, const auto& right) {
        if (const int order = compareFolders(left, right); order != 0) {
            return order < 0;
        }
        return utf8(left) < utf8(right);
    });
    std::vector<Shot> shots;
    for (const auto& parent : folders) {
        for (auto& shot : groupShots(std::move(byFolder[parent]))) {
            shots.push_back(std::move(shot));
        }
    }
    return shots;
}

} // namespace arraw::test::reference
