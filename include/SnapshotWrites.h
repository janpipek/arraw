#pragma once

#include <DevelopState.h>
#include <NegativeSettings.h>
#include <Sidecar.h>
#include <Snapshots.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace arraw {

/// @brief Snapshot to add, and the history position the session anchors it to.
struct SnapshotCreate {
    /// @brief Parent, name, time and recipe; the ID is allocated by the write.
    Snapshot draft;

    /// @brief Session position when the write was prepared; the write does not use it.
    std::size_t position = 0;
};

/// @brief New name of one snapshot.
struct SnapshotRename {
    /// @brief Snapshot to rename.
    SnapshotId id;

    /// @brief New name.
    std::string name;
};

/// @brief Snapshot to delete.
struct SnapshotDelete {
    /// @brief Snapshot to delete.
    SnapshotId id;
};

/// @brief Removal of the snapshot content this arraw cannot use
/// (::arraw::discardUnreadableSnapshots).
struct SnapshotDiscardUnreadable {};

/// @brief Rendered thumbnail of one snapshot, with the recipe its key is worked out from.
struct SnapshotThumbnailPatch {
    /// @brief Snapshot the thumbnail is of.
    SnapshotId id;

    /// @brief Thumbnail to store.
    SnapshotThumbnail thumbnail;

    /// @brief Develop state of the snapshot, as the session holds it.
    DevelopState state;

    /// @brief Negative settings of the snapshot, as the session holds them.
    NegativeSettings negative;
};

/// @brief Snapshot change for the sidecar, prepared by an ::arraw::EditSession and carried out on
/// any thread.
struct SnapshotWrite {
    /// @brief Photograph whose sidecar changes.
    std::filesystem::path photo;

    /// @brief Revision of the collection the session saw; ignored by a thumbnail patch.
    std::uint64_t expectedRevision = 0;

    /// @brief What to do.
    std::variant<SnapshotCreate, SnapshotRename, SnapshotDelete, SnapshotDiscardUnreadable,
                 SnapshotThumbnailPatch>
        change;
};

/// @brief What a snapshot write did.
struct SnapshotWriteOutcome {
    /// @brief Whether the sidecar changed.
    bool written = false;

    /// @brief The collection after a structural write; nothing for a thumbnail patch, and for a
    /// discard that found nothing to remove.
    std::optional<SnapshotUpdate> update;
};

/// @brief Carries out a snapshot write.
///
/// Safe from any thread: it reads and writes only the sidecar and, for a thumbnail patch, the
/// first and last 64 KiB of the photograph. A thumbnail patch works out the render key of its
/// recipe (::arraw::snapshotThumbnailKey) before the lock, and is refused, with nothing written,
/// when the key differs from the thumbnail's or the file cannot be looked at.
/// @param write Change to make.
/// @return What happened.
/// @throws what the sidecar writer throws: ::arraw::SnapshotRevisionConflict,
/// ::arraw::SidecarLockTimeout, std::invalid_argument, std::runtime_error. The sidecar is
/// unchanged then.
[[nodiscard]] SnapshotWriteOutcome writeSnapshotChange(const SnapshotWrite& write);

/// @brief Carries out several thumbnail patches of one photograph under one lock and one commit.
///
/// As ::arraw::writeSnapshotChange for each, with ::arraw::setSnapshotThumbnails. The keys are
/// worked out before the lock.
/// @param photo Photograph whose sidecar changes.
/// @param patches Patches to write.
/// @return For each patch, in order, whether it was written.
/// @throws std::invalid_argument, std::runtime_error, ::arraw::SidecarLockTimeout as
/// ::arraw::setSnapshotThumbnails.
[[nodiscard]] std::vector<bool>
writeSnapshotThumbnailPatches(const std::filesystem::path& photo,
                              std::span<const SnapshotThumbnailPatch> patches);

} // namespace arraw
