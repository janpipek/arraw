#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace arraw::app {

/// @brief Default most directories the film strip watches at once.
inline constexpr int defaultMaxWatchedFolders = 1000;

/// @brief Assignment of a limited number of directory watches, shallowest directories first.
///
/// The system's number of watches (inotify on Linux) is finite and shared with other programs, so
/// a big tree cannot be watched whole. The budget decides which directories get a watch: the
/// shallowest ones, and within a depth the first in ::arraw::folderPrecedes order, whatever the
/// order they were added in. It only keeps the books; the caller makes the system calls, and
/// reports one that the system refused.
///
/// Invariant: every watched directory comes before every waiting one, and as many are watched as
/// the cap allows. Directories the system refused are neither: they are set aside until the cap is
/// set anew.
class WatchBudget {
public:
    /// @brief What the caller must do about a directory just added.
    struct Grant {
        /// Whether the directory is to be watched.
        bool watch = false;
        /// A watched directory that gave its place to the new one, to be unwatched.
        std::optional<std::filesystem::path> evict;
    };

    /// @brief Directories that changed places when the cap or the directories changed.
    struct Moves {
        /// Directories that got a place: to be watched, and read once more.
        std::vector<std::filesystem::path> promoted;
        /// Directories that lost theirs: to be unwatched.
        std::vector<std::filesystem::path> evicted;
    };

    /// @brief Makes a budget.
    /// @param cap Most directories watched at once; negative counts as 0.
    explicit WatchBudget(int cap = defaultMaxWatchedFolders);

    /// @brief Adds a directory, which gets a place if it is shallower than the last one watched
    /// or there is room. Adding one known already changes nothing.
    /// @param path Directory.
    /// @param depth Depth below the root, 0 for the root.
    /// @return Whether to watch it, and which watched directory to release for it.
    Grant add(const std::filesystem::path& path, int depth);

    /// @brief Removes directories, and gives the places they leave to the waiting ones.
    /// @param paths Directories; unknown ones are ignored.
    /// @return The directories promoted to a place.
    std::vector<std::filesystem::path> remove(std::span<const std::filesystem::path> paths);

    /// @brief Removes one directory.
    /// @param path Directory.
    /// @return The directory promoted to its place, if any.
    std::optional<std::filesystem::path> remove(const std::filesystem::path& path);

    /// @brief Takes the system's refusal of a watch: the limit is reached, so the cap shrinks to
    /// the watches there are.
    ///
    /// The directory is set aside, to be tried again when the cap is set anew (the next walk). A
    /// place freed before then goes to a waiting directory, not to it: a refusal of this one
    /// directory alone (a share without change notification) must not take every freed place.
    /// @param path Directory the budget had granted a watch.
    void refused(const std::filesystem::path& path);

    /// @brief Sets the cap, as at the start of a walk (a limit that was reached may be lifted).
    /// @param cap Most directories watched at once; negative counts as 0.
    /// @return The directories that gained or lost a place.
    Moves setCap(int cap);

    /// @brief Forgets every directory and sets the cap.
    /// @param cap Most directories watched at once.
    void reset(int cap);

    /// @brief Tells whether a directory is to be watched.
    [[nodiscard]] bool isWatched(const std::filesystem::path& path) const;

    /// @brief Counts the directories to be watched.
    [[nodiscard]] int watched() const noexcept {
        return static_cast<int>(watched_.size());
    }

    /// @brief Counts the directories without a place: the waiting and the refused.
    [[nodiscard]] int unwatched() const noexcept {
        return static_cast<int>(waiting_.size() + refused_.size());
    }

    /// @brief Gives the cap now: the one set, or less after a refusal.
    [[nodiscard]] int cap() const noexcept {
        return cap_;
    }

private:
    /// @brief A directory in the order of watching.
    struct Key {
        int depth = 0;
        std::filesystem::path path;

        bool operator<(const Key& other) const;
    };

    /// @brief Fills the places that are free from the waiting directories.
    /// @return Those promoted.
    std::vector<std::filesystem::path> promote();

    /// @brief Releases the last watched directories until no more than the cap are.
    /// @return Those released.
    std::vector<std::filesystem::path> shrink();

    /// Order of directories in the index: any total order will do.
    struct PathLess {
        bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const {
            return a.native() < b.native();
        }
    };

    /// Places there are.
    int cap_;
    /// Depth of every directory known, watched or waiting.
    std::map<std::filesystem::path, int, PathLess> depths_;
    /// Directories to be watched.
    std::set<Key> watched_;
    /// Directories without a place, to be promoted when one is free.
    std::set<Key> waiting_;
    /// Directories the system refused a watch, set aside until the cap is set anew.
    std::set<Key> refused_;
};

} // namespace arraw::app
