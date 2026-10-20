#pragma once

#include <Shot.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <unordered_set>
#include <vector>

namespace arraw::app {

/// @brief Consecutive rows that change together.
struct RowRun {
    /// First row of the run.
    int first = 0;
    /// Number of rows, at least 1.
    int count = 0;

    friend bool operator==(const RowRun&, const RowRun&) = default;
};

/// @brief Most runs (inserts and removes together) one folder's merge applies row by row; a
/// bigger change is better shown as a reset.
inline constexpr int maxFolderRuns = 64;

/// @brief What turns the rows of one folder into its fresh listing.
///
/// Apply @c removes first, in the order given, then @c inserts in the order given; each then
/// leaves the rows exactly as the model signals need them. Then the rows in @c updates take the
/// new shot.
struct FolderDiff {
    /// @brief Runs of old rows that are gone, as old indices, last run first.
    ///
    /// Taking them in this order, an index is still valid when its run is removed.
    std::vector<RowRun> removes;

    /// @brief Runs of new rows to insert, as new indices, first run first.
    ///
    /// After the removals only the rows that stay are left, in order; inserting the runs in this
    /// order puts each at its final index.
    std::vector<RowRun> inserts;

    /// @brief New indices of the rows that stay but whose shot (the companions) changed.
    std::vector<int> updates;

    /// @brief Whether a row-by-row merge is not wanted: too many runs, or a changed order.
    bool needsReset = false;

    /// @brief Counts the runs.
    /// @return Removes and inserts together.
    [[nodiscard]] int runCount() const noexcept {
        return static_cast<int>(removes.size() + inserts.size());
    }

    /// @brief Tells whether the folder differs at all.
    /// @return False when the listing is the rows as they are.
    [[nodiscard]] bool changed() const noexcept {
        return needsReset || !removes.empty() || !inserts.empty() || !updates.empty();
    }
};

namespace detail {

/// @brief Hashes a path by value, from a pointer to it.
struct PathPtrHash {
    std::size_t operator()(const std::filesystem::path* path) const noexcept {
        return std::hash<std::filesystem::path>{}(*path);
    }
};

/// @brief Compares paths by value, from pointers to them.
struct PathPtrEqual {
    bool operator()(const std::filesystem::path* a, const std::filesystem::path* b) const noexcept {
        return *a == *b;
    }
};

/// Set of paths held by address; the paths must outlive it.
using PathPtrSet = std::unordered_set<const std::filesystem::path*, PathPtrHash, PathPtrEqual>;

/// @brief Appends a row to a list of runs, joining it to the last run when it adjoins.
inline void extendRun(std::vector<RowRun>& runs, int row) {
    if (!runs.empty() && runs.back().first + runs.back().count == row) {
        ++runs.back().count;
    } else {
        runs.push_back({row, 1});
    }
}

} // namespace detail

/// @brief Compares the rows of one folder with its fresh listing, in linear time.
///
/// Both sides hold the folder's shots in ::arraw::groupShots order, which is a total order on
/// the primary, so the rows that stay form the same subsequence on both sides. The rows whose
/// primary is not in the listing become remove runs, the listed shots that are not rows become
/// insert runs, and a staying row whose shot differs is an update. If the staying rows do not
/// come in the same order on both sides (which a sorted listing never causes) the diff asks for
/// a reset.
/// @tparam OldAt Callable taking a row index and returning a `const Shot&`.
/// @param oldCount Number of rows the folder has now.
/// @param oldAt Reads the shot of a row, 0 to @p oldCount - 1.
/// @param fresh The folder's shots now.
/// @param maxRuns Most runs to merge; more sets ::arraw::app::FolderDiff::needsReset.
/// @return The runs and updates.
template <typename OldAt>
[[nodiscard]] FolderDiff diffFolder(std::size_t oldCount, OldAt&& oldAt,
                                    std::span<const Shot> fresh, int maxRuns = maxFolderRuns) {
    FolderDiff diff;
    detail::PathPtrSet freshSet;
    freshSet.reserve(fresh.size());
    for (const Shot& shot : fresh) {
        freshSet.insert(&shot.primary);
    }
    detail::PathPtrSet oldSet;
    oldSet.reserve(oldCount);
    std::vector<std::size_t> kept;
    kept.reserve(oldCount);
    for (std::size_t i = 0; i < oldCount; ++i) {
        const Shot& shot = oldAt(i);
        oldSet.insert(&shot.primary);
        if (freshSet.contains(&shot.primary)) {
            kept.push_back(i);
        } else {
            detail::extendRun(diff.removes, static_cast<int>(i));
        }
    }
    std::ranges::reverse(diff.removes);

    std::size_t j = 0;
    for (const std::size_t i : kept) {
        const Shot& old = oldAt(i);
        while (j < fresh.size() && fresh[j].primary != old.primary) {
            if (oldSet.contains(&fresh[j].primary)) {
                diff.needsReset = true; // A staying row out of order.
                return diff;
            }
            detail::extendRun(diff.inserts, static_cast<int>(j));
            ++j;
        }
        if (j == fresh.size()) {
            diff.needsReset = true;
            return diff;
        }
        if (!(old == fresh[j])) {
            diff.updates.push_back(static_cast<int>(j));
        }
        ++j;
    }
    for (; j < fresh.size(); ++j) {
        detail::extendRun(diff.inserts, static_cast<int>(j));
    }
    diff.needsReset = diff.runCount() > maxRuns;
    return diff;
}

/// @brief Compares a vector of shots with their fresh listing.
/// @param old The folder's shots now.
/// @param fresh The folder's shots now on disk.
/// @param maxRuns Most runs to merge; more sets ::arraw::app::FolderDiff::needsReset.
/// @return The runs and updates, as ::arraw::app::diffFolder returns them.
[[nodiscard]] inline FolderDiff diffFolder(std::span<const Shot> old, std::span<const Shot> fresh,
                                           int maxRuns = maxFolderRuns) {
    return diffFolder(
        old.size(), [&](std::size_t i) -> const Shot& { return old[i]; }, fresh, maxRuns);
}

} // namespace arraw::app
