#include "SnapshotWrites.h"

#include <SnapshotThumbnails.h>

#include <string>
#include <type_traits>
#include <utility>

using namespace arraw;

std::vector<bool>
arraw::writeSnapshotThumbnailPatches(const std::filesystem::path& photo,
                                     std::span<const SnapshotThumbnailPatch> patches) {
    // Keys are worked out before the lock, so no file is read while it is held. A patch whose
    // key cannot be worked out, or is not its thumbnail's, is left out of the batch.
    std::vector<bool> written(patches.size(), false);
    std::vector<ThumbnailPatchByKey> batch;
    std::vector<std::size_t> origin;
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const SnapshotThumbnailPatch& patch = patches[i];
        validate(patch.thumbnail);
        const auto key = snapshotThumbnailKey(photo, patch.state, patch.negative);
        if (!key || *key != patch.thumbnail.renderKey) {
            continue;
        }
        batch.push_back({patch.id, patch.thumbnail, *key});
        origin.push_back(i);
    }
    if (batch.empty()) {
        return written;
    }
    const std::vector<bool> done = setSnapshotThumbnails(photo, batch);
    for (std::size_t i = 0; i < done.size(); ++i) {
        written[origin[i]] = done[i];
    }
    return written;
}

SnapshotWriteOutcome arraw::writeSnapshotChange(const SnapshotWrite& write) {
    return std::visit(
        [&](const auto& change) -> SnapshotWriteOutcome {
            using Change = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<Change, SnapshotCreate>) {
                return {true, addSnapshot(write.photo, write.expectedRevision, change.draft)};
            } else if constexpr (std::is_same_v<Change, SnapshotRename>) {
                return {true, renameSnapshot(write.photo, write.expectedRevision, change.id,
                                             change.name)};
            } else if constexpr (std::is_same_v<Change, SnapshotDelete>) {
                return {true, deleteSnapshot(write.photo, write.expectedRevision, change.id)};
            } else if constexpr (std::is_same_v<Change, SnapshotDiscardUnreadable>) {
                auto update = discardUnreadableSnapshots(write.photo, write.expectedRevision);
                const bool written = update.has_value();
                return {written, std::move(update)};
            } else {
                const SnapshotThumbnailPatch single[] = {change};
                return {writeSnapshotThumbnailPatches(write.photo, single).front(), std::nullopt};
            }
        },
        write.change);
}
