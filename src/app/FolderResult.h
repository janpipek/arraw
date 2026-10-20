#pragma once

#include <Shot.h>

#include <filesystem>
#include <vector>

namespace arraw::app {

/// @brief Spells a folder without a trailing separator, as its files' parent path spells it.
/// @param folder Folder, such as `/photos/` or `/photos`.
/// @return `/photos`; a root such as `/` stays as it is.
[[nodiscard]] inline std::filesystem::path withoutTrailingSeparator(std::filesystem::path folder) {
    if (!folder.has_filename() && folder.has_relative_path()) {
        folder = folder.parent_path();
    }
    return folder;
}

/// @brief One directory as a walk of the film strip's folder read it.
struct FolderResult {
    /// @brief Directory read, without a trailing separator.
    std::filesystem::path folder;

    /// @brief Its own shots, in ::arraw::groupShots order; none for a directory without any.
    std::vector<Shot> shots;
};

/// @brief A directory a walk read, and how deep it lies.
struct ListedDirectory {
    /// @brief Path of the directory, spelled as ::arraw::app::FolderResult::folder.
    std::filesystem::path path;

    /// @brief Depth below the root of the walk; 0 for the root.
    int depth = 0;
};

/// @brief What a finished walk saw.
struct ListingSummary {
    /// @brief Every directory read, the root first, in walk order (::arraw::folderPrecedes).
    std::vector<ListedDirectory> directories;

    /// @brief Number of shots listed.
    int shots = 0;

    /// @brief Number of directories that could not be read, the root among them if it failed.
    int unreadable = 0;
};

} // namespace arraw::app
