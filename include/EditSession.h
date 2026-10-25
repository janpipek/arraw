#pragma once

#include <DevelopState.h>
#include <LocalAdjustments.h>
#include <NegativeSettings.h>
#include <Photo.h>
#include <SettingDescriptors.h>
#include <SnapshotWrites.h>
#include <Snapshots.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arraw {

class LensDatabase;

/// @brief Kind of change that made a history step, beyond what its states show.
///
/// Front ends word a step from this, the detail and ::arraw::describeChange, so
/// the list never claims a change the states do not show (ADR 022).
enum class EditOrigin {
    Opened,  ///< The first step: the photograph as opened, or after discardChanges().
    Edit,    ///< An ordinary edit, worded from ::arraw::describeChange.
    Paste,   ///< Paste Settings.
    Preset,  ///< A preset applied; the detail is the preset's name.
    Reset,   ///< A reset to defaults.
    Crop,    ///< A crop-mode session committed as one step.
    Restore, ///< A snapshot restored; the detail is its name at the time.
};

/// @brief One entry of an edit history: the state it left the photograph in, and why.
struct HistoryStep {
    /// @brief Whole develop state after the step.
    DevelopState state;

    /// @brief Kind of change the step was.
    EditOrigin origin = EditOrigin::Edit;

    /// @brief What the origin applies to, such as a preset's name; empty otherwise.
    ///
    /// A name to show, not wording: the sentence is the front end's.
    std::string detail;

    /// @brief Negative settings after the step (ADR 049).
    ///
    /// Only ::arraw::EditSession::setNegative, the two-part ::arraw::EditSession::setState and
    /// ::arraw::EditSession::updateNegative change it from one step to the next.
    NegativeSettings negative;

    /// @brief Snapshot the working look was based on after the step, if any (ADR 056).
    ///
    /// Beside the state and the negative, so that undo and redo return the base too.
    /// ::arraw::EditSession::createSnapshot sets it on the current step without making a new one.
    std::optional<SnapshotId> baseSnapshot = std::nullopt;

    friend bool operator==(const HistoryStep&, const HistoryStep&) = default;
};

/// @brief What changed in one local adjustment that two develop states both hold.
struct MaskChange {
    /// @brief Id of the adjustment.
    LocalAdjustmentId id{};

    /// @brief Whether the name differs.
    bool name = false;

    /// @brief Whether it was turned on or off.
    bool enabled = false;

    /// @brief Whether the opacity differs.
    bool opacity = false;

    /// @brief Whether the weight was inverted or restored.
    bool invert = false;

    /// @brief Whether the shape differs: its geometry, or its kind. False for two brush masks,
    /// which differ in ::arraw::MaskChange::strokes.
    bool shape = false;

    /// @brief Whether the brush's strokes differ (both masks are brushes).
    bool strokes = false;

    /// @brief Keys of ::arraw::localAdjustmentDescriptors whose deltas differ, in table order.
    std::vector<std::string_view> deltas;

    friend bool operator==(const MaskChange&, const MaskChange&) = default;
};

/// @brief Local adjustments two develop states differ in, for naming a history step (ADR 044).
struct LocalChange {
    /// @brief Ids the second state holds and the first does not, in the second's order.
    std::vector<LocalAdjustmentId> added;

    /// @brief Ids the first state holds and the second does not, in the first's order.
    std::vector<LocalAdjustmentId> removed;

    /// @brief Whether the adjustments both hold are in another order.
    bool reordered = false;

    /// @brief The adjustments both hold that differ, in the second state's order.
    std::vector<MaskChange> changed;

    /// @brief Tells whether the two lists are the same.
    [[nodiscard]] bool empty() const noexcept {
        return added.empty() && removed.empty() && !reordered && changed.empty();
    }

    friend bool operator==(const LocalChange&, const LocalChange&) = default;
};

/// @brief Settings and local adjustments two develop states differ in, for naming a history step.
struct ChangeDescription {
    /// @brief Keys of ::arraw::developSettingDescriptors whose values differ, in table order.
    std::vector<std::string_view> keys;

    /// @brief Group every key belongs to; empty when there are no keys or they span groups.
    std::optional<SettingGroup> group;

    /// @brief Keys of the negative settings that differ, in the order `"demosaic"`,
    /// `"lensDistortion"`, `"lensVignetting"`, `"lensLateralCa"` and `"lens"` (the recorded lens);
    /// empty when the two negatives are equal. Filled only by the overload over history steps.
    std::vector<std::string_view> negative;

    /// @brief How the local adjustments differ; empty when they do not. The counter alone is not
    /// described.
    LocalChange local;

    friend bool operator==(const ChangeDescription&, const ChangeDescription&) = default;
};

/// @brief Lists the settings and the local adjustments two develop states differ in.
///
/// Compares each row of ::arraw::developSettingDescriptors through its
/// accessor, so a new setting is covered by its row alone. Rows of the
/// photograph's own settings, such as the grain seed, are compared like any
/// other: a step that changes only the seed still names it. The local part
/// matches adjustments by id (ADR 044): ids added, removed or reordered, and, for each id both
/// states hold, what differs in it.
/// @param before State a step started from.
/// @param after State it left.
[[nodiscard]] ChangeDescription describeChange(const DevelopState& before,
                                               const DevelopState& after);

/// @brief Lists what two history steps differ in: the develop state and the negative.
///
/// As the overload over states, plus ::arraw::ChangeDescription::negative (ADR 049).
/// @param before Step a change started from.
/// @param after Step it left.
[[nodiscard]] ChangeDescription describeChange(const HistoryStep& before, const HistoryStep& after);

/// @brief One photograph being edited: the document as it stands now, and how it got there.
///
/// A session exists only while a photograph is open, so there is no empty
/// session: "nothing open" is a caller holding no session, not a session
/// holding nothing. It owns no pixels; decoding and rendering belong to
/// whoever displays or exports the photograph (ADR 001).
///
/// It is where edits happen, as opposed to ::arraw::Photo, which is a value
/// that edits produce. An edit is opened with begin(), changed any number of
/// times with update() and closed with commit() or cancel(), so that a slider
/// drag or a brush stroke is one history step rather than a hundred (ADR 022).
/// History holds develop states and negative settings: marks are not undone. An open edit
/// remembers both, so update() and updateNegative() may change either and commit() makes one
/// step if either differs; cancel() restores both. setNegative() makes a step of its own
/// (ADR 049, 050).
///
/// History is a list of steps and a position in it, the opening state first.
/// Undo and redo move the position, and so does goTo(): navigating is not an
/// edit, so it adds nothing and drops nothing. The next edit after moving back
/// drops the steps beyond the position.
///
/// The session also holds the photograph as its sidecar holds it, saved(), so
/// that "unsaved changes" is a comparison and not a flag (ADR 030). Develop
/// edits reach the sidecar through save(); marks reach it at once, through
/// setMarks().
class EditSession {
public:
    /// @brief Starts editing a photograph with the snapshots its sidecar lists (ADR 056).
    ///
    /// History is just the opening state, with the saved base as its base. The session owns the
    /// collection from here on and keeps it current as it creates, renames and deletes.
    /// @param document Document to edit, from ::arraw::openDocument, which is taken to be what
    /// its sidecar holds (or defaults, when there is none).
    /// @param lensDatabase Database the session records lens matches in, as setLensDatabase().
    explicit EditSession(OpenDocument document, const LensDatabase* lensDatabase = nullptr)
        : photo_(document.photo), saved_(std::move(document.photo)),
          snapshots_(std::move(document.snapshots)), base_(document.savedBase),
          savedBase_(document.savedBase), unreadable_(document.unreadableSnapshots),
          history_{{saved_.state(), EditOrigin::Opened, {}, saved_.negative(), document.savedBase}},
          lensDatabase_(lensDatabase) {}

    /// @brief Starts editing a photograph that is known to have no snapshots.
    ///
    /// The same as the overload over a document with an empty collection and no base. A snapshot
    /// operation on a photograph whose sidecar does list some meets a revision conflict and
    /// refreshes the collection (see createSnapshot()).
    ///
    /// The session does not know the saved base the sidecar holds, so save() leaves it alone
    /// until the session creates a snapshot or restores one; use the overload over a document to
    /// have it kept.
    /// @param photo Document to edit, usually from ::arraw::openPhoto, which is
    /// taken to be what its sidecar holds (or defaults, when there is none).
    /// @param lensDatabase Database the session records lens matches in, as setLensDatabase().
    explicit EditSession(Photo photo, const LensDatabase* lensDatabase = nullptr)
        : EditSession(OpenDocument{std::move(photo), SnapshotCollection{}, std::nullopt, 0},
                      lensDatabase) {
        baseKnown_ = false;
    }

    /// @brief Sets the lens database the session records matches in.
    ///
    /// The session records the lens match itself (ADR 050): a step that changes the lens settings
    /// and leaves a correction on with no lens recorded records ::arraw::recordLensMatch in that
    /// same step. That is commit() (so begin(), goTo(), undo(), redo() and save() too),
    /// setNegative() and the two-part setState(), before they ask whether anything changed.
    /// update(), updateNegative(), cancel(), discardChanges() and navigation never record, and
    /// neither does a step that leaves the lens settings alone. A recorded lens, matched or
    /// chosen, is never replaced. With no database the step is made as given. Only the next step
    /// uses a new database: nothing already made or open is touched. Not thread-safe, like the
    /// rest of the session.
    /// @param database Database to match in, which must outlive the session or be detached first;
    /// null for none. Not owned.
    void setLensDatabase(const LensDatabase* database) noexcept {
        lensDatabase_ = database;
    }

    /// @brief Gives the lens database the session records matches in, or null.
    [[nodiscard]] const LensDatabase* lensDatabase() const noexcept {
        return lensDatabase_;
    }

    /// @brief Current state of the document, including an edit in progress.
    ///
    /// This is what to render: during a drag it already carries the latest update.
    [[nodiscard]] const Photo& photo() const noexcept {
        return photo_;
    }

    /// @brief Opens an edit, remembering the state it started from.
    ///
    /// An edit already open is committed first.
    void begin();

    /// @brief Changes the state provisionally, inside the open edit.
    /// @param state State the document carries from now on, until the next update.
    /// @throws std::logic_error if no edit is open.
    /// @throws std::invalid_argument if @p state is not valid; nothing changes.
    void update(DevelopState state);

    /// @brief Changes the negative settings provisionally, inside the open edit.
    ///
    /// For a slider that drags a negative setting, such as the vignetting amount (ADR 050): the
    /// drag is one history step with any develop updates. photo() carries the latest negative,
    /// and a caller that decodes compares it with the one its pixels were decoded with. The lens
    /// match is not recorded here, but by commit() (see setLensDatabase()).
    /// @param negative Negative settings the document carries from now on, until the next update.
    /// @throws std::logic_error if no edit is open.
    /// @throws std::invalid_argument if @p negative is not valid; nothing changes.
    void updateNegative(NegativeSettings negative);

    /// @brief Closes the open edit as one history step.
    ///
    /// An edit that ends where it started, in both the state and the negative, leaves no step. A
    /// new step drops every step after the position, so what could be redone is gone. A step that
    /// changes the lens settings records the lens match first (see setLensDatabase()), so an
    /// edit that only adds the record still makes a step. If matching throws, the edit stays open
    /// and the history is as it was.
    /// @param origin Kind of change the step is.
    /// @param detail What the origin applies to, such as a preset's name.
    /// @throws std::logic_error if no edit is open.
    void commit(EditOrigin origin = EditOrigin::Edit, std::string detail = {});

    /// @brief Closes the open edit, restoring the state and negative it started from.
    /// @throws std::logic_error if no edit is open.
    void cancel();

    /// @brief Whether an edit is open.
    [[nodiscard]] bool editing() const noexcept {
        return baseline_.has_value();
    }

    /// @brief Replaces the develop state as one history step.
    ///
    /// The same as begin(), update() and commit() in turn: for changes that
    /// happen at once, such as a reset or a choice from a list.
    /// @param state State the document carries from now on.
    /// @param origin Kind of change the step is.
    /// @param detail What the origin applies to, such as a preset's name.
    /// @throws std::invalid_argument if @p state is not valid; nothing changes.
    void setState(DevelopState state, EditOrigin origin = EditOrigin::Edit,
                  std::string detail = {});

    /// @brief Replaces the negative settings as one history step.
    ///
    /// An open edit is committed first. Setting the negative it already has leaves no step, after
    /// the lens match is recorded when the step changes the lens settings (see setLensDatabase()).
    /// A new step drops every step after the position. A caller that decodes (the window) compares
    /// photo().negative() with the one its pixels were decoded with after this, as after undo.
    /// @param negative Negative settings the document carries from now on.
    /// @param origin Kind of change the step is.
    /// @param detail What the origin applies to, such as a preset's name.
    /// @throws std::invalid_argument if @p negative is not valid; nothing changes.
    void setNegative(NegativeSettings negative, EditOrigin origin = EditOrigin::Edit,
                     std::string detail = {});

    /// @brief Replaces the negative settings and the develop state as one history step.
    ///
    /// For a paste or a preset, which carry both. An open edit is committed first; when neither
    /// part differs there is no step. The lens match is recorded as for setNegative(); if matching
    /// throws, the edit and the history stay as they were.
    /// @param negative Negative settings the document carries from now on.
    /// @param state State the document carries from now on.
    /// @param origin Kind of change the step is.
    /// @param detail What the origin applies to, such as a preset's name.
    /// @throws std::invalid_argument if either is not valid; nothing changes.
    void setState(NegativeSettings negative, DevelopState state,
                  EditOrigin origin = EditOrigin::Edit, std::string detail = {});

    /// @brief Steps taken so far, the opening state first.
    ///
    /// Steps after position() can be redone. While an edit is open they do not
    /// include it.
    [[nodiscard]] const std::vector<HistoryStep>& history() const noexcept {
        return history_;
    }

    /// @brief Index in history() of the step the document is at, when no edit is open.
    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

    /// @brief Moves to a step, keeping every step in the list.
    ///
    /// An open edit is committed first, and one that changed something drops
    /// the steps after the position, so @p index is taken after that commit.
    /// Navigating is not an edit: it adds no step and drops none.
    /// @param index Index in history() as it stands after the commit.
    /// @throws std::out_of_range if @p index is not in history(); nothing changes
    /// but the commit.
    void goTo(std::size_t index);

    /// @brief Whether undo() has a step to take back, counting an open edit that changed something.
    [[nodiscard]] bool canUndo() const noexcept;

    /// @brief Whether redo() has a step to bring back, after committing an open edit.
    [[nodiscard]] bool canRedo() const noexcept;

    /// @brief Takes back the latest history step.
    ///
    /// An open edit is committed first, so undoing in the middle of a drag
    /// reverts the drag and leaves it to redo.
    /// @throws std::logic_error if there is nothing to undo.
    void undo();

    /// @brief Brings back the step undo() last took back.
    ///
    /// An open edit is committed first, which clears what could be redone.
    /// @throws std::logic_error if there is nothing to redo.
    void redo();

    /// @brief Photograph as its sidecar holds it: the baseline of unsaved changes.
    [[nodiscard]] const Photo& saved() const noexcept {
        return saved_;
    }

    /// @brief Whether the develop state, the negative or the working base, an open edit
    /// included, differs from the saved one.
    ///
    /// A change of base with identical pixels counts: it needs saving. Marks never count: they
    /// are written as they change (see setMarks()).
    [[nodiscard]] bool hasUnsavedChanges() const {
        return photo_.state() != saved_.state() || photo_.negative() != saved_.negative() ||
               base_ != savedBase_;
    }

    /// @brief Snapshots of the photograph, as the session last read them from its sidecar or
    /// wrote them.
    ///
    /// The reference, and any pointer into it (from find()), is invalidated by the next create,
    /// rename, delete or refresh, and by a revision conflict: copy what must outlive them.
    [[nodiscard]] const SnapshotCollection& snapshots() const noexcept {
        return snapshots_;
    }

    /// @brief Count of raw snapshot entries the sidecar held, as the session last read or wrote
    /// it, that this arraw cannot use (::arraw::SidecarContents::unreadableSnapshots).
    [[nodiscard]] std::size_t unreadableSnapshots() const noexcept {
        return unreadable_;
    }

    /// @brief Snapshot the current look is based on, if any.
    [[nodiscard]] std::optional<SnapshotId> baseSnapshot() const noexcept {
        return base_;
    }

    /// @brief Snapshot the saved recipe is based on, if any.
    [[nodiscard]] std::optional<SnapshotId> savedBaseSnapshot() const noexcept {
        return savedBase_;
    }

    /// @brief Captures the current look as a new snapshot, written to the sidecar at once.
    ///
    /// Blocking: the write waits up to the lock timeout and parses the sidecar, so call it, like
    /// renameSnapshot(), deleteSnapshot() and refreshSnapshots(), from the thread that owns the
    /// session and expect it to take time. The synchronous snapshot methods are for the command
    /// line and scripts; the GUI uses the prepare, write and finish parts (prepareCreateSnapshot(),
    /// ::arraw::writeSnapshotChange, finishSnapshotWrite()) with a writer thread.
    ///
    /// A gesture under way is settled first, as save() does, with the lens match recorded. The
    /// snapshot holds the whole state and negative, takes the working base as its parent and a
    /// new ID from the sidecar. It is written with ::arraw::addSnapshot, so the saved settings and
    /// saved base on disk stay as they are. Then the new ID becomes the base of the current
    /// history position: no step is added and capturing drops nothing that could be redone
    /// (settling a gesture that changed something is an ordinary step, which does), so undo reaches
    /// the preceding edit and its earlier base, and redo returns to this position with the new
    /// base. The snapshot stays when the position is undone. If the write fails, nothing changes,
    /// an open edit included.
    /// @param name Name of the snapshot, 1 to ::arraw::maximumSnapshotNameBytes bytes.
    /// @return The new snapshot's ID.
    /// @throws std::invalid_argument if the name is invalid, or the list is full.
    /// @throws ::arraw::SnapshotRevisionConflict if the sidecar's snapshots changed since the
    /// session read them; the collection is then read again (and references to snapshots that are
    /// gone are remapped), but the capture is not retried.
    /// @throws std::runtime_error, ::arraw::SidecarLockTimeout as ::arraw::addSnapshot.
    SnapshotId createSnapshot(std::string name);

    /// @brief Switches the working look to a snapshot, as one undoable step.
    ///
    /// The step carries the snapshot's exact state and negative, bypassing lens rematching, and
    /// its ID as the base; the origin is ::arraw::EditOrigin::Restore and the detail the name. It
    /// is a step even when the rendering is unchanged, if the base differs. When both the look
    /// and the base already are the snapshot's there is no step. An open edit is committed first;
    /// a new step drops what could be redone.
    /// @param id Snapshot to restore.
    /// @throws std::invalid_argument if the session knows no such snapshot; nothing changes.
    void restoreSnapshot(SnapshotId id);

    /// @brief Renames a snapshot, writing the sidecar at once.
    ///
    /// Not a history step, and the rendering and the dirty state do not change. The collection is
    /// then the one the sidecar returns.
    /// @param id Snapshot to rename.
    /// @param name New name.
    /// @throws std::invalid_argument if the name is invalid or the session knows no such snapshot.
    /// @throws ::arraw::SnapshotRevisionConflict as createSnapshot().
    /// @throws std::runtime_error, ::arraw::SidecarLockTimeout as ::arraw::renameSnapshot.
    void renameSnapshot(SnapshotId id, std::string name);

    /// @brief Deletes a snapshot, writing the sidecar at once.
    ///
    /// Its children take its parent (or become roots), and every reference to it, the working
    /// base, the saved base, the open edit's and those of the history steps, becomes its parent
    /// or no base. Not a history step; the rendering is untouched. Undo does not bring it back.
    /// @param id Snapshot to delete.
    /// @throws std::invalid_argument if the session knows no such snapshot.
    /// @throws ::arraw::SnapshotRevisionConflict as createSnapshot().
    /// @throws std::runtime_error, ::arraw::SidecarLockTimeout as ::arraw::deleteSnapshot.
    void deleteSnapshot(SnapshotId id);

    /// @brief Removes the snapshot entries this arraw cannot use from the sidecar at once.
    ///
    /// Blocking, as createSnapshot(); the sync form of prepareDiscardUnreadableSnapshots(). Every
    /// snapshot the session lists is kept (::arraw::discardUnreadableSnapshots). A sidecar that
    /// holds nothing unusable is not written.
    /// @throws ::arraw::SnapshotRevisionConflict as createSnapshot().
    /// @throws std::runtime_error, ::arraw::SidecarLockTimeout as
    /// ::arraw::discardUnreadableSnapshots.
    void discardUnreadableSnapshots();

    /// @brief Prepares Create from the current look, for a writer thread to carry out.
    ///
    /// Pure: nothing changes in the session and nothing is read or written. The draft takes the
    /// working base as parent and records the history position for finishSnapshotWrite() to anchor
    /// to.
    /// @param name Name of the snapshot, 1 to ::arraw::maximumSnapshotNameBytes bytes.
    /// @return The write.
    /// @throws std::logic_error if an edit is open; the caller settles the gesture first.
    /// @throws std::invalid_argument if the name or the look is invalid.
    [[nodiscard]] SnapshotWrite prepareCreateSnapshot(std::string name) const;

    /// @brief Prepares Rename, for a writer thread to carry out; pure.
    /// @param id Snapshot to rename.
    /// @param name New name.
    /// @return The write.
    /// @throws std::invalid_argument if the name is invalid or the session knows no such snapshot.
    [[nodiscard]] SnapshotWrite prepareRenameSnapshot(SnapshotId id, std::string name) const;

    /// @brief Prepares Delete, for a writer thread to carry out; pure.
    /// @param id Snapshot to delete.
    /// @return The write.
    /// @throws std::invalid_argument if the session knows no such snapshot.
    [[nodiscard]] SnapshotWrite prepareDeleteSnapshot(SnapshotId id) const;

    /// @brief Prepares the removal of unusable snapshot entries, for a writer thread to carry out;
    /// pure.
    /// @return The write.
    [[nodiscard]] SnapshotWrite prepareDiscardUnreadableSnapshots() const;

    /// @brief Prepares storing a rendered thumbnail, for a writer thread to carry out; pure.
    ///
    /// Reads no file: the render key is worked out by ::arraw::writeSnapshotChange.
    /// @param id Snapshot the thumbnail is of.
    /// @param thumbnail Thumbnail to store.
    /// @return The write, or nothing when the session knows no such snapshot.
    /// @throws std::invalid_argument if @p thumbnail is invalid.
    [[nodiscard]] std::optional<SnapshotWrite>
    prepareSnapshotThumbnail(SnapshotId id, SnapshotThumbnail thumbnail) const;

    /// @brief Applies a snapshot write that succeeded.
    ///
    /// A Create adopts the collection and anchors the new snapshot to the current history step only
    /// if no edit is open and the session is still at the position, with the state, negative and
    /// base the write was prepared from; otherwise the snapshot is listed and the base is
    /// unchanged. Rename, Delete and discard adopt the collection, which remaps every reference to
    /// a deleted snapshot to its parent. A thumbnail that was written is stored on its snapshot
    /// when that is still listed; the history, bases, dirty state and revision never change. A
    /// write that was refused or found nothing to do changes nothing.
    /// @param write The write that was carried out.
    /// @param outcome What it did.
    /// @return False only for a Create that could not be anchored.
    /// @throws std::invalid_argument if @p write is not for this session's photograph.
    bool finishSnapshotWrite(const SnapshotWrite& write, const SnapshotWriteOutcome& outcome);

    /// @brief Takes a collection read elsewhere, after a conflict or an external change.
    ///
    /// Remaps every reference to a snapshot it no longer lists, as refreshSnapshots() does,
    /// without reading anything.
    /// @param fresh The sidecar's collection.
    /// @param unreadable Count of unusable raw entries beside it.
    void adoptSnapshots(SnapshotCollection fresh, std::size_t unreadable);

    /// @brief Reads the snapshots again from the sidecar, for a change another process made.
    ///
    /// References to snapshots that are gone (the working base, the saved base, the open edit's
    /// and the history's) become the nearest surviving ancestor in the collection the session
    /// held, or no base. The rendering and the dirty state of the settings do not change.
    /// The read takes no lock: every writer replaces the file atomically, so it sees one whole
    /// version.
    /// @throws std::runtime_error if the sidecar cannot be read; nothing changes.
    void refreshSnapshots();

    /// @brief Lists the snapshots whose thumbnail is missing, invalid (the reader drops those) or
    /// made for another recipe or file than the snapshot's now (ADR 056, decision 9).
    ///
    /// Each wants a regeneration; until it arrives the caller shows a placeholder. Reads the
    /// file's name and size only; changes nothing.
    /// @return The IDs, in the collection's order.
    [[nodiscard]] std::vector<SnapshotId> snapshotsNeedingThumbnails() const;

    /// @brief Stores a rendered thumbnail on its snapshot, if it is still the one wanted.
    ///
    /// The sync form of prepareSnapshotThumbnail(), ::arraw::writeSnapshotChange and
    /// finishSnapshotWrite().
    ///
    /// Works out the render key the snapshot's recipe has now (::arraw::snapshotThumbnailKey) and
    /// writes through ::arraw::setSnapshotThumbnail with it, so the thumbnail is accepted only if
    /// the snapshot still exists in the sidecar and the thumbnail was made for exactly this
    /// recipe and file. Only that node's thumbnail changes, in the sidecar and in snapshots(): the
    /// structural revision, the active settings, the history, the bases and the dirty state are
    /// not touched, and pending edits are not saved. Called from the thread that owns the session,
    /// like the other snapshot operations; it takes the sidecar lock (up to
    /// ::arraw::sidecarLockTimeoutMilliseconds). The write changes the sidecar's stamp: the caller
    /// must tell the folder watcher (::arraw::app::FilmStrip::noteOwnWrite) after a true return,
    /// or the write is reported as an external change.
    /// @param id Snapshot the thumbnail is of.
    /// @param thumbnail Thumbnail to store.
    /// @return False, with nothing changed, if the session knows no such snapshot, the key does
    /// not match, or the sidecar's snapshot is gone or its thumbnail changed meanwhile.
    /// @throws std::invalid_argument if @p thumbnail is invalid.
    /// @throws std::runtime_error, ::arraw::SidecarLockTimeout as ::arraw::setSnapshotThumbnail;
    /// nothing changed, and the settings never are.
    bool applySnapshotThumbnail(SnapshotId id, SnapshotThumbnail thumbnail);

    /// @brief Writes the photograph to its sidecar and makes that the saved state.
    ///
    /// Writes the settings, the negative and the working base together, and of the marks only
    /// those the session changed and has not written (none, as setMarks() writes at once), so a
    /// rating another process set since the session read the file is not put back. The snapshot
    /// collection on disk is left as it is. If another process deleted the base meanwhile, the
    /// collection is read again, the base remapped to the nearest survivor, and the save is tried
    /// once more; should that fail too, the refresh stands (the collection and the references to
    /// snapshots stay remapped) though nothing was saved.
    ///
    /// A session built from a bare Photo does not know the saved base, and leaves it as the file
    /// has it (and writes all its marks, as it always did) until it changes the base.
    ///
    /// An open edit is committed first. History is kept: saving is not an edit. With an open edit
    /// that is due a lens record, the file holds the photograph as the commit leaves it, with the
    /// match recorded, so nothing is unsaved straight after.
    /// @throws std::runtime_error as ::arraw::writeSidecar does; nothing is saved or changed.
    /// @throws ::arraw::SnapshotBaseMissing if the base is gone again on the second try.
    void save();

    /// @brief Sets the marks, writing them to the sidecar at once.
    ///
    /// Writes only the marks that differ from the saved ones, so develop edits not yet saved stay
    /// out of the file and so does a rating or recipe another process wrote since the session
    /// read it. Then updates saved() and photo(). This is not an undo step (ADR 021, 022).
    /// @param marks Marks the photograph carries from now on.
    /// @throws std::invalid_argument if @p marks are not valid.
    /// @throws std::runtime_error as ::arraw::writeSidecar does.
    /// Either way nothing changes.
    void setMarks(PhotoMarks marks);

    /// @brief Returns the develop state, the negative and the working base to the saved ones,
    /// dropping history and any open edit.
    ///
    /// History starts again from the saved state and base, as an Opened step. Snapshots written
    /// to the sidecar stay.
    void discardChanges();

private:
    Photo photo_;

    /// Photograph as the sidecar holds it.
    Photo saved_;

    /// Snapshots, as last read from or written to the sidecar.
    SnapshotCollection snapshots_;

    /// Snapshot the current look is based on.
    std::optional<SnapshotId> base_;

    /// Snapshot the saved recipe is based on.
    std::optional<SnapshotId> savedBase_;

    /// Raw snapshot entries the sidecar held that cannot be used, as last read or written.
    std::size_t unreadable_ = 0;

    /// Whether savedBase_ is what the sidecar holds; false for a session built from a bare Photo.
    bool baseKnown_ = true;

    /// What an open edit started from.
    struct Baseline {
        DevelopState state;             ///< Develop state at begin().
        NegativeSettings negative;      ///< Negative settings at begin().
        std::optional<SnapshotId> base; ///< Working base at begin().
    };

    /// Gives @p after with the lens match recorded when its lens settings differ from @p before.
    [[nodiscard]] Photo withLensRecorded(const LensSettings& before, const Photo& after) const;

    /// Makes @p next, which already carries any record, the photograph and ends the open edit.
    void commitPhoto(Photo next, EditOrigin origin, std::string detail);

    /// Makes the collection the session holds @p fresh and remaps every reference to a snapshot
    /// it no longer lists.
    void adopt(SnapshotCollection fresh);

    /// Gives the nearest ancestor of @p id, in the collection held now, that @p fresh still lists.
    [[nodiscard]] std::optional<SnapshotId> survivor(std::optional<SnapshotId> id,
                                                     const SnapshotCollection& fresh) const;

    /// Reads the collection again after a conflict, ignoring a failure to read.
    void refreshAfterConflict() noexcept;

    /// Builds the Create write for a draft taken from @p look, which must be the photograph as it
    /// will stand once any open edit is settled.
    [[nodiscard]] SnapshotWrite createWrite(std::string name, const Photo& look) const;

    /// Drops the steps after the position and appends one.
    void appendStep(HistoryStep step);

    /// Tells whether the open edit is back where it began. Requires an open edit.
    [[nodiscard]] bool baselineIsCurrent() const noexcept;

    /// Start of the open edit; empty when no edit is open.
    std::optional<Baseline> baseline_;

    /// Steps taken, the opening state first; never empty.
    std::vector<HistoryStep> history_;

    /// Index in history_ of the step the document is at, when no edit is open.
    std::size_t position_ = 0;

    /// Database the lens match is recorded in; not owned, may be null.
    const LensDatabase* lensDatabase_ = nullptr;
};

} // namespace arraw
