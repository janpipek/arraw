#pragma once

#include "FolderResult.h"
#include "ShotModel.h"

#include <Shot.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <utility>
#include <vector>

namespace arraw::test {

/// @brief Answers the walk a model last asked for at once, on this thread, as the lister would.
///
/// For the model's tests, which need neither a thread nor a watcher: the folder is read in the
/// lister's order (pre-order over ::arraw::listFolder), handed over as one batch, and the walk
/// is finished. A folder that cannot be read is skipped with its subtree, as the lister does.
/// @param model Model whose ::arraw::app::ShotModel::walk is answered.
inline void listNow(app::ShotModel& model) {
    std::vector<app::FolderResult> batch;
    app::ListingSummary summary;
    std::vector<app::ListedDirectory> pending{{app::withoutTrailingSeparator(model.folder()), 0}};
    while (!pending.empty()) {
        const app::ListedDirectory directory = std::move(pending.back());
        pending.pop_back();
        try {
            FolderListing listing = listFolder(directory.path);
            if (model.recursive()) {
                for (auto child = listing.folders.rbegin(); child != listing.folders.rend();
                     ++child) {
                    pending.push_back({*child, directory.depth + 1});
                }
            }
            summary.directories.push_back(directory);
            summary.shots += static_cast<int>(listing.shots.size());
            batch.push_back({directory.path, std::move(listing.shots)});
        } catch (const std::exception&) {
            ++summary.unreadable;
        }
    }
    const std::uint64_t walk = model.walk();
    model.applyFolders(walk, batch);
    model.finishListing(walk, summary);
}

} // namespace arraw::test
