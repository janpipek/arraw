#pragma once

#include <Shot.h>

#include <algorithm>
#include <filesystem>
#include <string>
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

/// @brief Order of folders as ::arraw::folderPrecedes gives it: a folder, then its subtree, then
/// its next sibling.
///
/// The one comparator of the film strip's rows and of the lister's tree (plan, Risks), so that a
/// folder's subtree is one contiguous range of either.
struct FolderOrder {
    /// @brief Tells whether @p a comes before @p b.
    bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const {
        return folderPrecedes(a, b);
    }
};

/// @brief Tells whether a path is a folder or lies below it, component by component.
/// @param path Path to look at, spelled as @p folder is (no trailing separator).
/// @param folder Folder.
/// @return True for @p folder itself and anything under it; false for `/a/bc` against `/a/b`.
[[nodiscard]] inline bool isWithin(const std::filesystem::path& path,
                                   const std::filesystem::path& folder) {
    const auto [inFolder, inPath] =
        std::mismatch(folder.begin(), folder.end(), path.begin(), path.end());
    return inFolder == folder.end();
}

/// @brief One directory as the film strip's lister read it, or found it gone.
struct FolderResult {
    /// @brief Directory read, without a trailing separator.
    std::filesystem::path folder;

    /// @brief Its own shots, in ::arraw::groupShots order; none for a directory without any.
    std::vector<Shot> shots;

    /// @brief Whether the directory went (gone, no longer a directory) or is refused for lack of
    /// permission, while the root still opens: its rows go, and so do those of every folder below
    /// it (::arraw::app::ShotModel::removeSubtree). A directory that fails in another way keeps
    /// its rows (see ::arraw::app::ListingSummary::stale).
    bool gone = false;

    /// @brief Whether the directory was read again because it reported a change, so that the
    /// sidecars of its rows that stay are checked too (a sidecar replaced by another program).
    bool recheck = false;
};

/// @brief What a finished walk saw.
struct ListingSummary {
    /// @brief Directories whose read failed in a way that may pass (the root, whatever the error,
    /// a subfolder with an I/O error, or any subfolder while the root does not open): their rows,
    /// and those below them, are kept, not swept as if they had gone
    /// (::arraw::app::ShotModel::finishListing). In no particular order.
    std::vector<std::filesystem::path> stale;
};

/// @brief How much of the tree the lister watches, as it stands once everything is read.
struct WatchStatus {
    /// @brief Number of directories under watch.
    int watched = 0;

    /// @brief Number of directories in the tree, watched or not.
    int directories = 0;

    /// @brief Number of subfolders that cannot be read now: dropped for lack of permission, or
    /// kept as stale after an I/O error and tried again at the next refresh. The root is not
    /// counted (see ::arraw::app::WatchStatus::rootUnavailable).
    int unreadable = 0;

    /// @brief The first few of those subfolders, for the notice's tooltip.
    std::vector<std::filesystem::path> unreadableFolders;

    /// @brief Whether the root itself cannot be read (unmounted, a share asleep): the rows are
    /// kept as they were, and come back to life when it can be read again.
    bool rootUnavailable = false;

    /// @brief The system's words for why the root cannot be read; empty while it can.
    std::string rootError;

    /// @brief Most directories the lister may watch now: the limit set, or less where the
    /// system refused a watch.
    int limit = 0;

    /// @brief Counts the directories without a watch, whose changes show on a refresh only.
    [[nodiscard]] int unwatched() const noexcept {
        return directories - watched;
    }

    bool operator==(const WatchStatus&) const = default;
};

} // namespace arraw::app
