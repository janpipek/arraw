#pragma once

#include <DevelopState.h>
#include <Diagnostics.h>
#include <NegativeSettings.h>
#include <Photo.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace arraw {

/// @brief Most snapshots one photograph keeps (ADR 056; provisional).
inline constexpr std::size_t maximumSnapshots = 32;

/// @brief Longest snapshot name, in UTF-8 bytes (ADR 056; provisional).
inline constexpr std::size_t maximumSnapshotNameBytes = 256;

/// @brief Longest side of a snapshot thumbnail, in pixels (ADR 056; provisional).
inline constexpr int maximumSnapshotThumbnailSide = 256;

/// @brief Longest thumbnail render key, in UTF-8 bytes.
inline constexpr std::size_t maximumSnapshotThumbnailKeyBytes = 256;

/// @brief Largest encoded snapshot thumbnail, in bytes (ADR 056; provisional).
inline constexpr std::size_t maximumSnapshotThumbnailBytes = 64 * 1024;

/// @brief Largest serialized XMP snapshot subtree, in UTF-8 bytes (ADR 056; provisional).
///
/// Counts JSON escaping, base64 and XML markup, and nothing outside the snapshot list.
inline constexpr std::size_t maximumSnapshotSubtreeBytes = 128ULL * 1024 * 1024;

/// @brief Identity of a snapshot within one photograph; zero is no snapshot.
struct SnapshotId {
    /// @brief Number; never reused after a snapshot is deleted.
    std::uint64_t value = 0;

    auto operator<=>(const SnapshotId&) const = default;
};

/// @brief A small rendering of a snapshot, derived from its recipe and replaceable.
struct SnapshotThumbnail {
    /// @brief Encoded JPEG, at most ::arraw::maximumSnapshotThumbnailSide pixels either way.
    std::vector<std::byte> jpeg;

    /// @brief Identity of the source, the snapshot's recipe and the thumbnail recipe it was made
    /// from.
    std::string renderKey;

    friend bool operator==(const SnapshotThumbnail&, const SnapshotThumbnail&) = default;
};

/// @brief A named, complete develop recipe of one photograph.
///
/// Immutable apart from its name, parent and thumbnail (ADR 056).
struct Snapshot {
    /// @brief Identity, nonzero.
    SnapshotId id;

    /// @brief Snapshot the working look was based on when this one was captured, if any.
    std::optional<SnapshotId> parent;

    /// @brief Name, 1 to ::arraw::maximumSnapshotNameBytes bytes of storable UTF-8; not unique.
    std::string name;

    /// @brief When it was captured, whole seconds, from 1970 to the end of the year 9999.
    std::chrono::sys_seconds created;

    /// @brief The develop state it restores.
    DevelopState state;

    /// @brief The negative settings it restores.
    NegativeSettings negative;

    /// @brief Rendering of it, when there is a usable one.
    std::optional<SnapshotThumbnail> thumbnail;

    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

/// @brief All snapshots of one photograph, in the order the sidecar lists them.
struct SnapshotCollection {
    /// @brief The snapshots, at most ::arraw::maximumSnapshots.
    std::vector<Snapshot> entries;

    /// @brief Identity the next added snapshot takes; above every identity, and never lowered.
    std::uint64_t nextId = 1;

    /// @brief Count of structural changes (add, rename, delete) written to the sidecar.
    ///
    /// A thumbnail write does not change it.
    std::uint64_t revision = 0;

    /// @brief Finds a snapshot.
    /// @param id Identity to look for.
    /// @return The snapshot, or null when there is none of that identity.
    [[nodiscard]] const Snapshot* find(SnapshotId id) const noexcept;

    friend bool operator==(const SnapshotCollection&, const SnapshotCollection&) = default;
};

/// @brief A structural change (add, rename, delete) that cannot be made because the sidecar's
/// collection is no longer the one the caller saw.
class SnapshotRevisionConflict : public std::runtime_error {
public:
    /// @brief Makes the error.
    /// @param expected Revision the caller saw.
    /// @param actual Revision the sidecar holds now.
    SnapshotRevisionConflict(std::uint64_t expected, std::uint64_t actual);

    /// @brief Revision the caller saw.
    std::uint64_t expected;

    /// @brief Revision the sidecar holds now.
    std::uint64_t actual;
};

/// @brief Checks a thumbnail.
///
/// A render key of 1 to ::arraw::maximumSnapshotThumbnailKeyBytes bytes of storable UTF-8, and a
/// JPEG whose header is readable, no more than
/// ::arraw::maximumSnapshotThumbnailBytes long and ::arraw::maximumSnapshotThumbnailSide pixels
/// on each side.
/// @param thumbnail Thumbnail to check.
/// @throws std::invalid_argument naming what is wrong.
void validate(const SnapshotThumbnail& thumbnail);

/// @brief Checks a snapshot on its own.
///
/// A nonzero id that is not its own parent, a storable name of 1 to
/// ::arraw::maximumSnapshotNameBytes bytes, a time the sidecar can spell, a valid state and
/// negative, and a valid thumbnail if it has one.
/// @param snapshot Snapshot to check.
/// @throws std::invalid_argument naming what is wrong.
void validate(const Snapshot& snapshot);

/// @brief Checks a collection.
///
/// Each snapshot by ::arraw::validate(const Snapshot&), at most ::arraw::maximumSnapshots of
/// them, no identity repeated, `nextId` above every identity, every parent present and no cycle.
/// @param collection Collection to check.
/// @throws std::invalid_argument naming what is wrong.
void validate(const SnapshotCollection& collection);

/// @brief A photograph with everything its sidecar adds to it.
struct OpenDocument {
    /// @brief The photograph, as ::arraw::openPhoto gives it.
    Photo photo;

    /// @brief The snapshots, empty when the sidecar has none.
    SnapshotCollection snapshots;

    /// @brief Snapshot the saved recipe was based on, if the sidecar says and it still exists.
    std::optional<SnapshotId> savedBase;

    /// @brief Count of raw snapshot entries the sidecar holds that this arraw cannot use.
    ///
    /// As ::arraw::SidecarContents::unreadableSnapshots; they are kept in the file until
    /// ::arraw::discardUnreadableSnapshots removes them.
    std::size_t unreadableSnapshots = 0;
};

/// @brief Opens a photograph with its snapshots, reading the image metadata and the sidecar once.
///
/// As ::arraw::openPhoto, which is this without the snapshots. A damaged snapshot list does not
/// stop the photograph opening: what cannot be used is reported (::arraw::Notice::SnapshotDamaged)
/// and left out, as ::arraw::readSidecar describes.
/// @param path File to open.
/// @param log Where to report what a photographer should know about the file.
/// @return The document.
/// @throws std::runtime_error as ::arraw::openPhoto.
[[nodiscard]] OpenDocument openDocument(const std::filesystem::path& path,
                                        DiagnosticLog& log = discardedDiagnostics());

/// @brief Gives a photograph the look of a snapshot: the one path every surface selects a
/// snapshot for rendering or export through (ADR 056, decision 10).
///
/// The snapshot's state and negative are copied exactly: no lens is rematched, nothing is read
/// from the disk or written to it. The file and marks stay the photograph's own, so explicit
/// overrides apply to the result as to any photograph.
/// @param photo Photograph to take the file, metadata and marks from.
/// @param snapshot Snapshot to take the state and negative from.
/// @return A document over the same file, developed as the snapshot says.
/// @throws std::invalid_argument as the ::arraw::Photo constructor does.
[[nodiscard]] Photo withSnapshot(const Photo& photo, const Snapshot& snapshot);

} // namespace arraw
