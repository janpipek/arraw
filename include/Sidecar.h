#pragma once

#include <DevelopState.h>
#include <Diagnostics.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <PhotoMarks.h>
#include <Snapshots.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arraw {

/// @brief Namespace of the develop settings in an XMP sidecar.
///
/// Frozen: it carries no version, which `arraw:version` holds (ADR 051). Sidecars of the builds
/// that spelled it `http://ns.arraw.org/develop/1.0/` are read as this and written back as this.
inline constexpr std::string_view sidecarNamespace = "http://ns.arraw.org/develop/";

/// @brief Prefix the develop settings get when a sidecar has to declare their namespace.
inline constexpr std::string_view sidecarPrefix = "arraw";

/// @brief Newest version of the sidecar this arraw reads and writes.
///
/// Version 2 adds snapshots and `arraw:activeStatePresent`, and every write of this arraw is
/// version 2 (ADR 056, ADR 051's amendment).
inline constexpr int sidecarVersion = 2;

/// @brief Time a writer waits for another writer's lock on a sidecar, in milliseconds.
inline constexpr int sidecarLockTimeoutMilliseconds = 2000;

/// @brief Age after which a writer breaks another writer's lock on a sidecar, in milliseconds.
///
/// A lock file whose owner is gone, or that is not refreshed, is taken over after this long. Set
/// explicitly rather than left to Qt's default.
inline constexpr int sidecarStaleLockMilliseconds = 5000;

/// @brief The sidecar's lock could not be taken in time, so nothing was written.
class SidecarLockTimeout : public std::runtime_error {
public:
    /// @brief Makes the error.
    /// @param sidecar Path of the sidecar that stayed locked.
    /// @param lockFile Path of the lock file that was held.
    SidecarLockTimeout(const std::filesystem::path& sidecar, const std::string& lockFile);
};

/// @brief The saved base of a Save no longer names a snapshot of the sidecar, so nothing was
/// written.
///
/// Another process deleted it after the caller last read the list. The caller reads the sidecar
/// again, remaps its base and saves once more.
class SnapshotBaseMissing : public std::invalid_argument {
public:
    /// @brief Makes the error.
    /// @param base Snapshot the caller named.
    /// @param sidecar Path of the sidecar that lacks it.
    SnapshotBaseMissing(SnapshotId base, const std::filesystem::path& sidecar)
        : std::invalid_argument("the saved base " + std::to_string(base.value) +
                                " is not a snapshot of " + sidecar.string()),
          base(base) {}

    /// @brief Snapshot the caller named.
    SnapshotId base;
};

/// @brief A namespace other tools left properties in, as a sidecar shows it.
struct ForeignNamespace {
    /// @brief Namespace URI.
    std::string uri;

    /// @brief Prefix of its first counted property, empty for the default namespace.
    std::string prefix;

    /// @brief Number of top-level properties of the `rdf:Description`s in this namespace.
    int properties = 0;

    friend bool operator==(const ForeignNamespace&, const ForeignNamespace&) = default;
};

/// @brief What an XMP sidecar holds that arraw understands, and who else wrote in it.
struct SidecarContents {
    /// @brief Develop state the sidecar records, or nothing when it records none.
    ///
    /// A sidecar records a state when it has arraw's develop settings: an
    /// `arraw:version` or any settings key. Each key it leaves out keeps the
    /// neutral default of a default-constructed state, so a sidecar written
    /// before a setting existed renders as it did. A sidecar of marks alone, or
    /// one only another tool wrote in, records none, and the photograph starts
    /// from what its kind does (::arraw::defaultStateFor), as with no sidecar.
    std::optional<DevelopState> state;

    /// @brief Negative settings the sidecar records, or nothing when it has none of their keys
    /// (`arraw:demosaic` and the `arraw:lens...` ones).
    ///
    /// Independent of ::arraw::SidecarContents::state: a sidecar with only `arraw:demosaic` or
    /// `arraw:lensDistortion` records a negative and no state. A token that is not known is
    /// reported and read as the default.
    std::optional<NegativeSettings> negative;

    /// @brief Culling marks, none for whatever the sidecar does not say.
    PhotoMarks marks;

    /// @brief Snapshots the sidecar lists, empty when it lists none (ADR 056).
    ///
    /// Read tolerantly: a node that cannot be used is left out, an invalid thumbnail is dropped,
    /// a parent that is missing makes a root, an edge that closes a cycle is cut, and `nextId` is
    /// raised above every id read, each with ::arraw::Notice::SnapshotDamaged.
    SnapshotCollection snapshots;

    /// @brief Snapshot the saved active recipe is based on, when it exists in
    /// ::arraw::SidecarContents::snapshots.
    std::optional<SnapshotId> savedBase;

    /// @brief Whether the snapshot list holds something this reading left out or repaired
    /// (a node that cannot be used, a list that is not a sequence), other than a thumbnail.
    ///
    /// The snapshot writers keep such content verbatim, or refuse the edit when they cannot.
    bool snapshotsDamaged = false;

    /// @brief Count of raw snapshot entries this reading cannot use.
    ///
    /// The nodes it leaves out (damaged, a repeated id, past ::arraw::maximumSnapshots) plus,
    /// when the list is not a single sequence, the items in all its occurrences (at least 1).
    /// These stay in the file and count toward the limit until
    /// ::arraw::discardUnreadableSnapshots removes them.
    std::size_t unreadableSnapshots = 0;

    /// @brief Program that wrote the sidecar, from `xmp:CreatorTool`.
    std::optional<std::string> creatorTool;

    /// @brief Namespaces of other tools, in the order of their first counted property.
    ///
    /// Every namespace with a property on an `rdf:Description` other than arraw's,
    /// `rdf:`, `x:`, `xmp:` and the XML ones, so `dc:` is listed. Only top-level
    /// properties are counted, not what a property holds. Among namespaces that
    /// appear only as attributes of one description, the order is by attribute name.
    std::vector<ForeignNamespace> others;

    friend bool operator==(const SidecarContents&, const SidecarContents&) = default;
};

/// @brief A known XMP namespace and the tool or standard behind it.
struct XmpNamespaceOwner {
    /// @brief Namespace URI.
    std::string_view uri;

    /// @brief Name of the tool or vocabulary.
    std::string_view name;
};

/// @brief Namespaces ::arraw::xmpNamespaceOwner knows.
inline constexpr auto xmpNamespaceOwners = std::to_array<XmpNamespaceOwner>({
    {"http://ns.adobe.com/camera-raw-settings/1.0/",
     "Adobe Camera Raw / Lightroom develop settings"},
    {"http://ns.adobe.com/lightroom/1.0/", "Lightroom hierarchical keywords"},
    {"http://ns.adobe.com/photoshop/1.0/", "Photoshop / IPTC fields"},
    {"http://ns.adobe.com/xap/1.0/mm/", "XMP media management"},
    {"http://ns.adobe.com/exif/1.0/aux/", "EXIF auxiliary (copied)"},
    {"http://cipa.jp/exif/1.0/", "EXIF extended (copied)"},
    {"http://ns.adobe.com/xap/1.0/rights/", "XMP rights"},
    {"http://iptc.org/std/Iptc4xmpExt/2008-02-29/", "IPTC Extension"},
    {"http://ns.microsoft.com/photo/1.2/", "Microsoft Photo (people regions)"},
    {"http://www.metadataworkinggroup.com/schemas/regions/", "MWG regions"},
    {"http://darktable.sf.net/", "darktable"},
    {"http://www.digikam.org/ns/1.0/", "digiKam"},
    {"http://ns.microsoft.com/photo/1.0/", "Windows Photo Gallery"},
    {"http://ns.adobe.com/exif/1.0/", "EXIF (copied)"},
    {"http://ns.adobe.com/tiff/1.0/", "TIFF (copied)"},
    {"http://purl.org/dc/elements/1.1/", "Dublin Core"},
    {"http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/", "IPTC Core"},
});

/// @brief Names the tool or standard behind an XMP namespace.
/// @param uri Namespace URI, compared exactly.
/// @return Its human name, or nothing for a namespace not in ::arraw::xmpNamespaceOwners.
[[nodiscard]] std::optional<std::string_view> xmpNamespaceOwner(std::string_view uri);

/// @brief Names the sidecar of a photograph.
///
/// The sidecar is `<stem>.xmp` beside the photograph, as Lightroom names it. A
/// photograph that shares its stem with another image in the same directory
/// gets `<name>.<extension>.xmp` instead, so that a pair never share one; the
/// exception is a RAW, which keeps `<stem>.xmp` unless another RAW shares its
/// stem. The RAW extensions are those LibRaw opens, and the other images are
/// found by probing the usual extensions in lower and upper case, so the cost
/// does not grow with the directory. When `<name>.XMP` exists and `<name>.xmp`
/// does not, that file is named. Looks at the directory; nothing is created.
/// @param photo Path of the photograph.
/// @return Path of its sidecar, which need not exist.
[[nodiscard]] std::filesystem::path sidecarPath(const std::filesystem::path& photo);

/// @brief Reads the sidecar of a photograph, if it has one.
///
/// `arraw:demosaic` is the photograph's negative (ADR 049): one of
/// ::arraw::demosaicTokens. An unknown token gives ::arraw::Notice::SettingMalformed and the
/// default, AHD. The lens corrections are `arraw:lensDistortion`, `arraw:lensVignetting` and
/// `arraw:lensLateralCa`. The first and last are `True` or `False`; `lensVignetting` is an amount
/// in percent of the profile, a number from 0 to 150 (a number outside is clamped with
/// ::arraw::Notice::SettingClamped), and the legacy `True` and `False` read as 100 and 0. Absent
/// means off. The lens they were found for
/// is `arraw:lensMaker`, `arraw:lensModel`, `arraw:lensCropFactor`, `arraw:lensOrigin`
/// (`matched` or `chosen`) and `arraw:lensDatabase` together (ADR 050, section 8). A value that
/// is none of these gives ::arraw::Notice::SettingMalformed and off; a record with no model or no
/// positive crop factor gives it and no recorded lens; an unknown origin gives it and `matched`.
///
/// **Version 2** (ADR 056) adds `arraw:activeStatePresent` (`True` or `False`: whether the file
/// records develop settings; version 1 files keep their rule that `arraw:version` alone does), and
/// the snapshots: `arraw:savedBase`, `arraw:snapshotNextId`, `arraw:snapshotRevision` and
/// `arraw:snapshots`, an `rdf:Seq` of items with the fields `id`, `parent`, `name`, `created`
/// (ISO 8601), `state` and `negative` (the JSON documents of ::arraw::stateToJson and
/// ::arraw::negativeToJson), `thumbnail` (base64 JPEG) and `thumbnailKey`. A version 2 file whose
/// marker is missing or malformed is reported (::arraw::Notice::SettingMalformed) and records a
/// state only if it has active develop keys. Snapshot reading is tolerant: see
/// ::arraw::SidecarContents::snapshots.
///
/// Every `rdf:Description` is searched. The develop settings are the
/// `arraw:` properties, as attributes or as simple child elements, applied onto
/// defaults through the same codec as the JSON document, so a value out of
/// range is clamped, and an unknown key or a value of the wrong shape is
/// skipped, each with a warning that names the sidecar. So are `xmp:Rating`
/// (a whole number, `3.0` included, clamped to -1 to 5) and `xmp:Label`. A
/// rating or label that does not fit is reported and read as no mark; the file
/// keeps it (see ::arraw::writeSidecar). Whether the file records develop
/// settings at all is ::arraw::SidecarContents::state. Arraw content without an `arraw:version`
/// is version 1, with no warning. Every version up to
/// ::arraw::sidecarVersion is read; an `arraw:version` above it gives
/// ::arraw::Notice::NewerSettingsVersion and is read as far as it is understood (ADR 051).
///
/// **Local adjustments** (ADR 044) are two more `arraw:` properties: `arraw:localAdjustments`,
/// an `rdf:Seq` of structures (read in any of RDF's equivalent spellings:
/// `rdf:parseType="Resource"`, a nested `rdf:Description`, fields as attributes), and
/// `arraw:nextLocalAdjustmentId`. The `arraw:localAdjustmentsVersion` of earlier builds is ignored.
/// Only the description's own properties are settings, so a mask's `arraw:exposure` is never the
/// global one. A mask that cannot be used is dropped, with
/// ::arraw::Notice::LocalAdjustmentDropped, and a field that is not known ignored, with
/// ::arraw::Notice::LocalAdjustmentFieldIgnored; the counter is raised above every id kept. The
/// rules are those of ::arraw::applyStateJson. Either property records a state.
/// `crs:` and anything else is not read, but it is reported: `xmp:CreatorTool`
/// and the other namespaces that hold properties (see ::arraw::SidecarContents).
/// @param photo Path of the photograph, which is also the subject of every warning.
/// @param log Where the warnings go.
/// @return What the sidecar holds, or nothing when there is no sidecar.
/// @throws std::runtime_error if the sidecar cannot be read or is not XML,
/// naming it.
[[nodiscard]] std::optional<SidecarContents>
readSidecar(const std::filesystem::path& photo, DiagnosticLog& log = discardedDiagnostics());

/// @brief Writes the develop settings and marks of a photograph into its sidecar.
///
/// Like every sidecar writer it takes `<sidecar>.lock` (a QLockFile) for up to
/// ::arraw::sidecarLockTimeoutMilliseconds, rereads the latest file, patches only the fields it
/// owns and replaces the file atomically; it writes version 2. The snapshot list and the saved base
/// are left as the file has them (see the overload that sets the base).
///
/// An existing sidecar is edited rather than replaced: everything arraw does
/// not own, such as `crs:` settings, other namespaces, unknown elements and
/// unknown `arraw:` attributes, is kept. Every settings key and `arraw:version`
/// becomes an attribute of the `rdf:Description` that already holds `arraw:`
/// content, or of the first one; an unset optional has no attribute, and a
/// child-element form of any key arraw owns is removed, so the file says each
/// thing once. `xmp:Rating` and `xmp:Label` are written only when the
/// photograph's marks differ from what reading the file gives, so a rating or
/// label arraw cannot represent (`9`, `Rot`) stays as another tool wrote it
/// until the mark is changed; clearing the label removes it. `arraw:demosaic` and the lens keys
/// follow the same rule, key by key: each is written only when the photograph's negative differs
/// from what reading the file gives, so a token of a newer arraw survives, and a default (AHD,
/// a switch that is off, no recorded lens) is written as no key. The lens record is written and
/// removed as a unit. No coefficient is written: the calibration stays the database's.
/// ::arraw::writeSidecarMarks leaves the negative alone. The local adjustments are
/// rewritten whole: `arraw:localAdjustments` and `arraw:nextLocalAdjustmentId` are written when
/// the list is not empty or the counter is above 1, and removed otherwise, so a state with no
/// masks and a fresh counter leaves no trace and every state reads back equal. A sidecar of an
/// older version is overwritten as the current one; one of a newer version is refused. Without a
/// sidecar, a new one is created. The file is replaced atomically. What is preserved is the meaning
/// of the document, not its bytes: attributes may be reordered, the packet's padding and trailing
/// newline are dropped, and the declaration is rewritten as UTF-8.
/// @param photo Photograph whose settings and marks to write.
/// @throws std::runtime_error if the existing sidecar is not XMP, or is of a
/// newer ::arraw::sidecarVersion (writing would destroy what a newer arraw wrote), naming it,
/// in which case it is left alone, or if the file cannot be written.
/// @throws ::arraw::SidecarLockTimeout if another writer holds the lock for too long.
void writeSidecar(const Photo& photo);

/// @brief Writes a photograph as ::arraw::writeSidecar does, and the saved working base with it.
/// @param photo Photograph whose settings and marks to write.
/// @param savedBase Snapshot the saved recipe is based on, or nothing for none; it must exist in
/// the sidecar's list when the lock is held.
/// @throws ::arraw::SnapshotBaseMissing (an std::invalid_argument) if @p savedBase names no
/// snapshot of the sidecar; nothing is written, not even the settings.
/// @throws std::runtime_error as the overload without a base.
void writeSidecar(const Photo& photo, std::optional<SnapshotId> savedBase);

/// @brief Writes only the marks of a photograph into its sidecar.
///
/// As ::arraw::writeSidecar does for the marks, and leaves the develop settings, the snapshots,
/// `arraw:version` and everything foreign as the file has them. Without a sidecar a new one is
/// created that holds the marks alone, so it records no develop state and the photograph still
/// starts from what its kind does (::arraw::defaultStateFor). This is how a session writes a
/// rating without saving the edits it has not saved (ADR 030). Version 2 is written whatever the
/// file was; whether it records develop settings is kept in `arraw:activeStatePresent` from the
/// rule of the version it had, so a RAW with no saved settings is not turned into one with
/// explicit neutral settings.
/// @param photo Path of the photograph.
/// @param marks Marks to write.
/// @throws std::invalid_argument if the rating of @p marks is outside -1 to 5.
/// @throws std::runtime_error as ::arraw::writeSidecar does: for an existing
/// sidecar that is not XMP or is of a newer version, which is left alone, or
/// that cannot be read, and for a file that cannot be written.
/// @throws ::arraw::SidecarLockTimeout if another writer holds the lock for too long.
void writeSidecarMarks(const std::filesystem::path& photo, const PhotoMarks& marks);

/// @brief The marks a writer means to change, and no others.
///
/// A session that writes the whole of its marks would put back a rating another process set
/// since the session read the file. Naming the changed marks lets a stale session leave the rest
/// alone. A mark named here is still written only when it differs from what the file says now.
struct MarkChanges {
    /// @brief New rating, or nothing to leave the file's rating alone.
    std::optional<int> rating = std::nullopt;

    /// @brief New label, or nothing to leave the file's label alone. An engaged empty optional
    /// clears the label.
    std::optional<std::optional<ColorLabel>> label = std::nullopt;

    /// @brief Whether it names no mark.
    [[nodiscard]] bool empty() const noexcept {
        return !rating && !label;
    }
};

/// @brief Lists the marks that differ between two sets.
/// @param from Marks the caller last knew the file to hold.
/// @param to Marks the caller wants.
/// @return The rating if it differs and the label if it differs.
[[nodiscard]] MarkChanges changedMarks(const PhotoMarks& from, const PhotoMarks& to);

/// @brief Writes a photograph as ::arraw::writeSidecar does, the saved working base with it, and
/// only the marks named.
///
/// For a session's ordinary Save: the marks of @p photo are ignored, and just @p marks reach the
/// file, so a rating another process wrote since the session read the file survives.
/// @param photo Photograph whose settings to write.
/// @param savedBase Snapshot the saved recipe is based on, or nothing for none.
/// @param marks Marks to change.
/// @throws ::arraw::SnapshotBaseMissing if @p savedBase names no snapshot of the sidecar; nothing
/// is written.
/// @throws std::invalid_argument if a rating of @p marks is outside -1 to 5.
/// @throws std::runtime_error as ::arraw::writeSidecar.
void writeSidecar(const Photo& photo, std::optional<SnapshotId> savedBase,
                  const MarkChanges& marks);

/// @brief Writes only the named marks of a photograph, as ::arraw::writeSidecarMarks does.
/// @param photo Path of the photograph.
/// @param marks Marks to change; nothing is written (not even a sidecar made) when it is empty.
/// @throws std::invalid_argument if a rating of @p marks is outside -1 to 5.
/// @throws std::runtime_error as ::arraw::writeSidecarMarks.
void writeSidecarMarkChanges(const std::filesystem::path& photo, const MarkChanges& marks);

/// @brief What a snapshot edit gives back.
struct SnapshotUpdate {
    /// @brief The collection as it now stands in the sidecar.
    SnapshotCollection collection;

    /// @brief The saved base as it now stands.
    std::optional<SnapshotId> savedBase;

    /// @brief The snapshot added, or the one renamed or deleted; zero for a discard.
    SnapshotId id;

    /// @brief Count of raw entries the sidecar holds that this arraw cannot use, as the reader
    /// sees the list after the edit.
    std::size_t unreadable = 0;
};

/// @brief Adds a snapshot to the sidecar, without saving anything else.
///
/// Rereads the sidecar under its lock, checks the revision, allocates the id from the counter in
/// the file, appends the node and raises the counter and the revision. The active settings, the
/// negative, the marks and the saved base stay as they are; without a sidecar one is created that
/// records no develop state. The node's id in @p draft is ignored.
/// @param photo Path of the photograph.
/// @param expectedRevision Revision of the collection the caller saw.
/// @param draft Snapshot to add: parent, name, time, recipe and thumbnail.
/// @return The collection after the edit and the new id.
/// @throws std::invalid_argument if @p draft is invalid, its parent does not exist, the list is
/// full, or the subtree would pass ::arraw::maximumSnapshotSubtreeBytes.
/// @throws ::arraw::SnapshotRevisionConflict if the revision in the file is not
/// @p expectedRevision.
/// @throws std::runtime_error if the snapshot list holds content this arraw cannot keep whole, or
/// as ::arraw::writeSidecar.
SnapshotUpdate addSnapshot(const std::filesystem::path& photo, std::uint64_t expectedRevision,
                           Snapshot draft);

/// @brief Renames a snapshot in the sidecar; as ::arraw::addSnapshot otherwise.
/// @param photo Path of the photograph.
/// @param expectedRevision Revision of the collection the caller saw.
/// @param id Snapshot to rename.
/// @param name New name.
/// @throws std::invalid_argument if the name is invalid or no snapshot has the id.
/// @throws ::arraw::SnapshotRevisionConflict and std::runtime_error as ::arraw::addSnapshot.
SnapshotUpdate renameSnapshot(const std::filesystem::path& photo, std::uint64_t expectedRevision,
                              SnapshotId id, std::string name);

/// @brief Deletes a snapshot from the sidecar; as ::arraw::addSnapshot otherwise.
///
/// Its children take its parent, or become roots; a saved base that was the snapshot becomes its
/// parent. Settings are not touched.
/// @param photo Path of the photograph.
/// @param expectedRevision Revision of the collection the caller saw.
/// @param id Snapshot to delete.
/// @throws std::invalid_argument if no snapshot has the id.
/// @throws ::arraw::SnapshotRevisionConflict and std::runtime_error as ::arraw::addSnapshot.
SnapshotUpdate deleteSnapshot(const std::filesystem::path& photo, std::uint64_t expectedRevision,
                              SnapshotId id);

/// @brief Attaches or replaces the thumbnail of one snapshot.
///
/// Under the lock, finds the node as the file has it now; unless it exists and
/// @p expectedKey is the render key @p thumbnail carries, nothing is written. The caller works out
/// @p expectedKey before the call, from the recipe it holds for the snapshot now, so no caller
/// code runs under the lock. A thumbnail made for a recipe the caller has since replaced is
/// dropped this way. Does not change the revision or any other node, so it never undoes a
/// structural change made meanwhile.
/// @param photo Path of the photograph.
/// @param id Snapshot the thumbnail is of.
/// @param thumbnail Thumbnail to store.
/// @param expectedKey Render key the snapshot's thumbnail ought to carry now.
/// @return Whether the thumbnail was written.
/// @throws std::invalid_argument if @p thumbnail is invalid.
/// @throws std::runtime_error as ::arraw::addSnapshot.
bool setSnapshotThumbnail(const std::filesystem::path& photo, SnapshotId id,
                          SnapshotThumbnail thumbnail, std::string_view expectedKey);

/// @brief One thumbnail to attach, with the render key the caller expects the snapshot to have.
struct ThumbnailPatchByKey {
    /// @brief Snapshot the thumbnail is of.
    SnapshotId id;

    /// @brief Thumbnail to store.
    SnapshotThumbnail thumbnail;

    /// @brief Render key the snapshot's thumbnail ought to carry now.
    std::string expectedKey;
};

/// @brief Attaches or replaces the thumbnails of several snapshots under one lock.
///
/// As ::arraw::setSnapshotThumbnail for each patch, with one lock, one reread of the file and one
/// commit: a patch whose snapshot is gone, or whose expected key is not the one its thumbnail
/// carries, is skipped, and nothing is written when none matches. The subtree limit is checked
/// once, for the result. The revision is never changed.
/// @param photo Path of the photograph.
/// @param patches Thumbnails to store; the later of two for one snapshot wins.
/// @return For each patch, in order, whether it was written.
/// @throws std::invalid_argument if a thumbnail is invalid (nothing is written) or the result
/// would pass ::arraw::maximumSnapshotSubtreeBytes.
/// @throws std::runtime_error, ::arraw::SidecarLockTimeout as ::arraw::setSnapshotThumbnail.
std::vector<bool> setSnapshotThumbnails(const std::filesystem::path& photo,
                                        std::span<const ThumbnailPatchByKey> patches);

/// @brief Removes from the sidecar the snapshot content this arraw cannot use, keeping every
/// snapshot it lists.
///
/// The only writer that drops raw snapshot content (ADR 056, decision 15); every other writer
/// keeps it or refuses the edit. Under the lock it rereads the sidecar and checks the revision.
/// It removes every raw entry the reader leaves out: damaged nodes, repeated ids and entries past
/// ::arraw::maximumSnapshots; when the list is not a single sequence it removes every occurrence
/// of the property. It writes each usable node's parent as the reader repaired it (a missing
/// parent or a cut cycle edge), removes a saved base that names no listed snapshot, raises the
/// revision, and sets the counter above every raw id seen in all occurrences, so no id is ever
/// reused. Usable nodes, and foreign content in them, are patched in place, not rewritten. If a
/// reread would still report unusable content, nothing is written.
/// @param photo Path of the photograph.
/// @param expectedRevision Revision of the collection the caller saw.
/// @return The sidecar's snapshots afterwards, or nothing, with no write at all, when it held
/// nothing unusable.
/// @throws ::arraw::SnapshotRevisionConflict if the revision in the file is not
/// @p expectedRevision.
/// @throws ::arraw::SidecarLockTimeout, std::runtime_error as ::arraw::addSnapshot.
std::optional<SnapshotUpdate> discardUnreadableSnapshots(const std::filesystem::path& photo,
                                                         std::uint64_t expectedRevision);

} // namespace arraw
