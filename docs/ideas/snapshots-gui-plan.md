# Snapshots Gate 4: GUI, writer thread and damaged lists (plan)

Status: **proposed**, 2026-10-25. Gates 1 to 3 are committed (`747ac79`, `5b1c1bc`, `96e9ee6`).
This plan covers Gate 4 of [`todo.md`](../todo.md) and the items deferred to it, and builds on
[ADR 056](../adr/056-snapshots-are-per-photo-looks-with-independent-history-and-sidecar-storage.md)
and the [architecture proposal](snapshots-architecture-proposal.md). The decisions it adds are
recorded in the addendum in section 7, which is appended to ADR 056. It contains no production
code.

## 1. Decisions

### User decisions (2026-10-25, final)

| # | Question | Answer |
|---|---|---|
| 1 | Create with identical pixels leaves the session dirty (base differs from the saved base) | Left to the architect. Choice: accept it, because it follows ADR 056 decision 3 and needs no extra write or ADR change. Ctrl+S saves the base, and the Save prompt is reworded (section 3.7) |
| 2 | Thread for snapshot sidecar writes (create, rename, delete, thumbnail) | A dedicated writer thread, not the UI thread |
| 3 | Snapshot thumbnail jobs and the developed thumbnails of the visible shots | Snapshot jobs go **behind** the visible developed jobs |
| 4 | Where the snapshot tree goes | The **same dock** as History, above the history list |

### Architect's choices (each with its reason)

- **Each session operation is split into three parts: prepare on the UI thread (pure), write on
  any thread (I/O only), finish on the UI thread.** The core free functions (`addSnapshot`,
  `renameSnapshot`, `deleteSnapshot`, `setSnapshotThumbnail`) are already I/O that does not touch
  a session, so the split moves no logic. It only separates "decide" from "apply".
- **The synchronous `EditSession` methods stay** (`createSnapshot`, `renameSnapshot`,
  `deleteSnapshot`, `applySnapshotThumbnail`). They are rebuilt as prepare, write, finish. The
  CLI, Python (Gate 5) and the Gate 2 and 3 tests keep their semantics, which are already tested.
- **The writer is one `std::jthread` with a FIFO queue and a callback** (`SnapshotWriter`,
  `src/app`). It works the way `ThumbnailWorker` does: it needs no event loop and tests can
  drive it without a window.
- **The UI does not lock editing while a structural write runs.** Instead, Create's finish
  anchors the new ID only if the session is still at the captured position with the captured look
  and base. Writes usually take milliseconds, and greying out the panel for each one would flicker.
- **At most one structural write is in flight per window.** The snapshot actions are disabled
  until its result lands, so the expected revision is always the one the UI shows. Without this,
  two quick Creates would conflict with each other.
- **Ordinary Save and marks writes stay synchronous on the UI thread.** The user decision covers
  snapshot writes. Save is correct in any interleaving with the writer: it keeps the collection,
  and it retries once after `SnapshotBaseMissing`.
- **Thumbnail patches are batched per photograph.** Adjacent queued patches for one photograph
  are applied under one lock with one commit (new `setSnapshotThumbnails`). This turns up to 32
  rewrites into one and keeps the time the lock is held short, so it does not compete with Save
  (Gate 3 review, finding 4).
- **No render key is worked out on the UI thread.** The key now reads 128 KiB of the raw file
  per snapshot. The thumbnail worker skips a job whose key equals the stored thumbnail's key, and
  the writer computes the expected key before it calls the patch.
- **The writer records the sidecar stamp right after each commit**, and the watcher remembers
  the last four own stamps of each sidecar. Two own writers (the writer thread and a UI-thread
  Save) can note their stamps out of order.
- **Preview uses a "shown photo" that is separate from the session photo.** Rendering, decoding
  and the panel's display read the shown photo; edits read the session's. Nothing in the session
  changes during a preview.
- **Rename and Create ask for the name with `QInputDialog`.** A new dialog class is not worth it
  for one line edit.
- **The default name for a new snapshot is the local short date and time.** It needs no counter
  and tells snapshots apart even when names repeat.
- **Delete asks for confirmation.** It is durable and cannot be undone. **Restore does not ask**,
  because it is an undoable step. Double-click and Enter on a row restore.
- **Ctrl+N creates a snapshot.** It is free in the app and is Lightroom's key for New Snapshot.

## 2. Writer thread (STEP A)

### 2.1 Core: a write request as a value (`include/SnapshotWrites.h`, new)

```cpp
namespace arraw {

/// @brief Snapshot to add, and the history position the session anchors it to.
struct SnapshotCreate {
    Snapshot draft;            ///< Parent, name, time, recipe; the ID is allocated by the write.
    std::size_t position = 0;  ///< EditSession::position() when prepared; not used by the write.
};

/// @brief New name of one snapshot.
struct SnapshotRename { SnapshotId id; std::string name; };

/// @brief Snapshot to delete.
struct SnapshotDelete { SnapshotId id; };

/// @brief Removal of the snapshot content this arraw cannot use (section 4).
struct SnapshotDiscardUnreadable {};

/// @brief Rendered thumbnail of one snapshot, with the recipe its key must be worked out from.
struct SnapshotThumbnailPatch {
    SnapshotId id;
    SnapshotThumbnail thumbnail;
    DevelopState state;        ///< The snapshot's recipe as the session holds it.
    NegativeSettings negative;
};

/// @brief A snapshot change for the sidecar, prepared by an EditSession, to write on any thread.
struct SnapshotWrite {
    std::filesystem::path photo;
    std::uint64_t expectedRevision = 0;  ///< Ignored by a thumbnail patch.
    std::variant<SnapshotCreate, SnapshotRename, SnapshotDelete, SnapshotDiscardUnreadable,
                 SnapshotThumbnailPatch> change;
};

/// @brief What a snapshot write did.
struct SnapshotWriteOutcome {
    bool written = false;                  ///< Whether the sidecar changed.
    std::optional<SnapshotUpdate> update;  ///< The collection after a structural write.
};

/// @brief Carries out a snapshot write. Safe from any thread; reads and writes only the sidecar.
///
/// A thumbnail patch works out the render key from the patch's recipe (this reads the photo's
/// first and last 64 KiB) and is refused, not written, when the key differs. Nothing runs under
/// the lock but the core writer.
/// @throws what the underlying writer throws (SnapshotRevisionConflict, SidecarLockTimeout,
/// std::invalid_argument, std::runtime_error); the sidecar is unchanged then.
[[nodiscard]] SnapshotWriteOutcome writeSnapshotChange(const SnapshotWrite& write);

} // namespace arraw
```

Reason for `std::variant`: each kind carries only its own fields, and the writer's `std::visit`
is the single place that dispatches. No class hierarchy is involved.

Core additions beside it:

- `SnapshotUpdate::unreadable` (`std::size_t`): the raw entries the reader leaves out after the
  write.
- `SidecarContents::unreadableSnapshots` and `OpenDocument::unreadableSnapshots`, defined in
  section 4.
- `std::vector<bool> setSnapshotThumbnails(const std::filesystem::path& photo,
  std::span<const ThumbnailPatchByKey> patches)`, where `ThumbnailPatchByKey` is
  `{SnapshotId id; SnapshotThumbnail thumbnail; std::string expectedKey;}`. It uses one lock, one
  reread and patches each node that still exists and whose expected key the thumbnail carries. It
  checks the subtree size once (Gate 1 review, finding 8), and commits once, or not at all when
  nothing matched. `setSnapshotThumbnail` becomes a one-element wrapper. The revision is never
  changed.

### 2.2 Core: `EditSession` prepare and finish

```cpp
/// Prepares Create from the current look. Requires no open edit (std::logic_error); the caller
/// settles a gesture first. Validates the draft (std::invalid_argument). No I/O.
[[nodiscard]] SnapshotWrite prepareCreateSnapshot(std::string name) const;
[[nodiscard]] SnapshotWrite prepareRenameSnapshot(SnapshotId id, std::string name) const;
[[nodiscard]] SnapshotWrite prepareDeleteSnapshot(SnapshotId id) const;
[[nodiscard]] SnapshotWrite prepareDiscardUnreadableSnapshots() const;
/// Nothing when the session knows no such snapshot. Validates the thumbnail header. No I/O.
[[nodiscard]] std::optional<SnapshotWrite> prepareSnapshotThumbnail(SnapshotId id,
                                                                     SnapshotThumbnail thumbnail) const;

/// Applies a write that succeeded. Returns false only for a Create the session could not anchor.
/// @throws std::invalid_argument if write.photo is not this session's photograph.
bool finishSnapshotWrite(const SnapshotWrite& write, const SnapshotWriteOutcome& outcome);

/// Takes a collection read elsewhere (after a conflict, or an external change) and remaps every
/// reference to a snapshot it no longer lists, as refreshSnapshots() does, without reading.
void adoptSnapshots(SnapshotCollection fresh, std::size_t unreadable);

/// Counts the raw snapshot entries the sidecar holds that this arraw cannot use.
[[nodiscard]] std::size_t unreadableSnapshots() const noexcept;
```

`finishSnapshotWrite`, by kind:

- **Create:** adopt `update.collection`, then anchor `update.id` to the current step if
  `!editing()`, `position() == create.position`, and the photo's state and negative and the
  working base equal the draft's state, negative and parent. Return whether it anchored. If it
  did not, the snapshot exists, it is listed, and the base is unchanged. This keeps ADR 056's
  anchoring exact when the user edited, undid or restored while the write ran.
- **Rename, Delete, DiscardUnreadable:** adopt `update.collection` and `update.unreadable`.
  Adopting the post-delete collection remaps the working base, the saved base, the baseline and
  the history to the parent, through `survivor()` over the held collection. This is exactly
  `replaceBase(id, parent)`, because the revision matched.
- **Thumbnail patch, written:** set that node's thumbnail if the ID is still listed. The recipe
  of an ID never changes, so no key check is needed here.
- **Not written** (a refused thumbnail): nothing changes.

The semantics of Gates 2 and 3 are kept as follows:

- **Atomic on failure:** a failed write is never finished, and prepare mutates nothing, so a
  failure leaves the session unchanged, an open edit included.
- **Revision conflict:** the writer returns the collection it read right after the conflict, and
  the UI calls `adoptSnapshots`. The synchronous path keeps `refreshAfterConflict()`.
- **`refreshSnapshots()`** stays for the synchronous API and becomes a read followed by
  `adoptSnapshots`.
- **The synchronous methods** are prepare, `writeSnapshotChange`, then finish.
  `createSnapshot` keeps its gesture handling: it drafts from the settled photo, writes, commits
  the gesture, then anchors, as today. Every test in `test_SessionSnapshots.cpp` passes unchanged.

### 2.3 App: `SnapshotWriter` (`src/app/SnapshotWriter.{h,cpp}`, `arraw-app-core`)

```cpp
namespace arraw::app {

/// @brief One write the writer carried out or failed, delivered on the writer thread.
struct SnapshotWriteResult {
    std::uint64_t ticket = 0;            ///< From submit().
    std::uint64_t owner = 0;             ///< Caller's tag (the window's session serial).
    SnapshotWrite write;                 ///< The request, echoed.
    SnapshotWriteOutcome outcome;        ///< Valid when failure is None.
    enum class Failure { None, Conflict, LockTimeout, Refused, Error } failure = Failure::None;
    std::string error;                   ///< what() of the failure.
    std::optional<SnapshotUpdate> fresh; ///< Conflict: the collection read right after.
    std::optional<std::int64_t> stamp;   ///< fileStamp of the sidecar right after a write.
};

/// @brief Thread that writes snapshot changes to sidecars, one at a time, in order.
class SnapshotWriter {
public:
    /// @param onResult Called on the writer thread for each request, in submission order; the
    /// caller marshals it. Must not throw. Never called once destruction has begun.
    explicit SnapshotWriter(std::function<void(SnapshotWriteResult)> onResult);
    /// Stops delivering, drops queued thumbnail patches, carries out the queued structural writes
    /// (each bounded by the lock timeout), and joins.
    ~SnapshotWriter();
    std::uint64_t submit(SnapshotWrite write, std::uint64_t owner);
    [[nodiscard]] std::size_t pending() const;
    /// Test seam: called on the writer thread before each write.
    void setBeforeWriteHook(std::function<void()> hook);
};

} // namespace arraw::app
```

- **Owner:** `MainWindow` holds it by value. It is declared last so it is destroyed first. Its
  callback posts with `QMetaObject::invokeMethod(this, …, Qt::QueuedConnection)`, the way
  `FilmStrip` posts thumbnail results.
- **Ordering:**
  1. One thread and one FIFO queue. Results are delivered in submission order, and queued calls
     to one receiver keep that order.
  2. Adjacent `SnapshotThumbnailPatch` requests for the same photo, up to 32, are coalesced into
     one `setSnapshotThumbnails` call. Each request still gets its own result, and they share the
     stamp. A structural request is never reordered past another request.
  3. The window allows at most one structural request in flight (section 3.4).
  4. Save and marks writes on the UI thread take the same `QLockFile` as the writer. In-process
     contention between threads behaves as it does between processes, and the threaded Gate 1
     tests rely on it; STEP A adds a test for a Save waiting on the writer.
- **Errors:** `SnapshotRevisionConflict` becomes Conflict, and the writer then reads
  `readSidecar(photo)` with its own local `CollectedDiagnostics` (not the shared discarding log),
  to fill `fresh`. `SidecarLockTimeout` becomes LockTimeout. `std::invalid_argument` (a full
  list, an invalid name) becomes Refused. Anything else becomes Error. Nothing escapes the thread.
- **Stamp:** after any write with `written == true`, the writer reads
  `arraw::app::fileStamp(sidecarPath(photo))` on its own thread.
- **Shutdown:** the destructor first sets `delivering = false`. Queued structural writes are user
  intent and durable, so they run. Their failures go to `qWarning`, because no window is left to
  show them. Thumbnail patches are derived data and are dropped.
- **Lock:** the writer holds the lock only inside the core calls. A batch is bounded at 32
  patches, about 2 MiB of base64, so it stays far below the 5 s stale-lock time.

### 2.4 Other STEP A changes

- **`ThumbnailWorker` priority:** urgent, then embedded (visible first), then **developed
  visible**, then snapshot jobs, then the rest of the developed jobs (user decision 3). The
  class documentation is updated to match.
- **`ThumbnailWorker::requestSnapshotThumbnail(primary, recipe, generation, storedKey)`:** the
  worker computes the key first and drops the job with no result when it equals `storedKey`
  (empty means none is stored). The window can then request every snapshot at open without
  reading the raw file on the UI thread.
- **`FolderWatcher::noteOwnWrite(sidecar, stamp)` overload:** `ownStamps_` keeps the last four
  stamps per sidecar, and `onSidecarRefreshed` matches any of them. The existing overload reads
  the stamp now and adds it.

## 3. GUI (STEP B)

### 3.1 Dock layout

The `HistoryDock` widget becomes a `QSplitter(Qt::Vertical)` (object name `historySplitter`),
which the user can resize:

1. The snapshot section (`snapshotPanel`) contains:
   - a header row: the label "Snapshots", a `+` tool button for Create, and a menu button with
     Rename, Delete, Restore and "Remove Unreadable Entries…";
   - a damaged-list notice (`snapshotDamageNotice`, hidden unless
     `session.unreadableSnapshots() > 0`): "N snapshot entries in the sidecar cannot be read."
     with a **Remove…** button;
   - a preview banner (`snapshotPreviewBanner`, hidden unless previewing): "Previewing “name”"
     with **Restore** and **Close** buttons;
   - a `QTreeView` (`snapshotTree`) with a 64 px icon size, no header, `expandAll()` after each
     reset, and single selection.
2. The existing `historyList`, which is unchanged.

The dock's toggle keeps its object name `presetsAndHistoryAction` and is retitled
"&Snapshots and History". Presets retitle it again when they arrive.

Enablement replaces today's whole-dock switch. The snapshot section is enabled when pixels are
loaded and the window is neither in the crop mode nor in the mask mode, which avoids two meanings
of Esc. The history list is enabled when `editable()` holds (section 3.3) and the window is not in
the crop mode.

### 3.2 `SnapshotModel` (`src/app/SnapshotModel.{h,cpp}`, `arraw-app-core`)

This is a `QAbstractItemModel` tree over a copy of the session's collection, holding at most 32
nodes. It has no session pointer, because the session's references are invalidated by every
write.

- **Input:** `setSnapshots(const SnapshotCollection&, std::optional<SnapshotId> base,
  std::optional<SnapshotId> savedBase)`.
  - The model resets when the set of IDs or any parent changed, and the caller then restores
    the selection by ID.
  - Otherwise it emits `dataChanged` for each changed row, so a thumbnail or a rename keeps the
    selection.
  - `clear()` empties the model.
  - `setRendered(SnapshotId, QImage)` shows a freshly rendered image before its write lands.
- **Structure:** a node's parent is its `parent` ID. Roots and children are in collection
  order, which is creation order. `index(row, column, parent)` and `parent(index)` come from a
  per-node vector of children that the model rebuilds on reset.
- **Roles:**

  | Role | Value |
  |---|---|
  | `Qt::DisplayRole` | Name |
  | `Qt::DecorationRole` | `QImage`, fitted to 96 px. Taken from the node's JPEG, decoded once per ID and render key, else from `setRendered`, else a neutral grey placeholder. A pending render and a failed one look the same (accepted) |
  | `Qt::FontRole` | Bold for the working base |
  | `Qt::ToolTipRole` | Created time (locale long format), the change from the parent, and "Saved base" when it is |
  | `IdRole` | `quint64` |
  | `ParentIdRole` | `quint64`, 0 for a root |
  | `CreatedRole` | `QDateTime` |
  | `ChangeRole` | `QString`, the edge's wording; empty for a root, "No changes" for an equal recipe |
  | `BaseRole` | `bool`, the working base |
  | `SavedBaseRole` | `bool` |

- **Edge wording:** `describeChange(HistoryStep{parent.state, Edit, {}, parent.negative},
  HistoryStep{node.state, Edit, {}, node.negative})` is worded by the function History uses.
  STEP B extracts the `EditOrigin::Edit` branch of `HistoryModel::textOf` into a free function
  `QString wordEdit(const HistoryStep& before, const HistoryStep& after)` in `HistoryModel.h`.
  `HistoryModel` and `SnapshotModel` both call it, so the two lists never word a change
  differently. A delete reparents children and the reset recomputes their edges.
- **No editing through the model:** `setData` is not implemented. Renames go through the
  writer, and the model shows the new name when the result is adopted.

### 3.3 Preview and Restore

- **`OpenPhoto` gains** `std::optional<SnapshotId> previewed`, `std::optional<Photo>
  previewPhoto`, `std::uint64_t serial` (from a window counter, set at each `open_.emplace`),
  `std::uint64_t snapshotGeneration`, `std::optional<std::uint64_t> structuralTicket` and
  `bool thumbnailWritesFailed`.
- **Shown photo:** `MainWindow::shownPhoto()` returns `*previewPhoto` while previewing, else
  `session.photo()`. STEP B classifies each of the 37 `session.photo()` uses in `MainWindow.cpp`.
  Rendering, `syncDecode`, the panel's `showState`, the mask sync and the lens status read
  `shownPhoto()`; edits keep reading the session's photo. A preview whose negative needs other
  decode options re-decodes, and so does closing it. This is accepted, because demosaic
  differences between looks are rare.
- **Editing gate:** `editable()` becomes `open_ && open_->decoded && !previewing()`. A new
  `hasPixels()` keeps today's meaning for view-only controls: zoom and fit (line 620 today),
  clipping, the histogram and the film strip. STEP B audits all 16 `editable()` sites. While
  previewing, the develop dock, the history list, crop, masks, the picker, paste, copy,
  undo/redo, geometry commands, Create and Save are disabled. Save is disabled because it stores
  the working look, not the one on screen. Marks, navigation, zoom and export (the saved settings,
  unchanged) stay enabled.
- **Start a preview:** selecting a row does the following in order: `finishPendingEdit()` (on
  failure, refuse and clear the selection), build `session.photo().with(s.negative).with(s.state)`
  (on `std::invalid_argument`, refuse with a status message), set the preview, then refresh the
  panel. If the window is in the crop mode, the tree is disabled, so no preview can start there.
- **Close a preview:** Esc (a `QShortcut` enabled only while previewing), the banner's
  **Close**, clearing the selection, switching photo, or a reset that drops the previewed ID. All
  of them clear the preview and refresh the panel.
- **Restore:** the banner's **Restore**, the menu's Restore, or double-click or Enter on a row.
  It closes the preview, calls `session.restoreSnapshot(id)` (no I/O), then `refreshPanel()` and
  `updateHistoryDock()`. It is allowed while a structural write runs; Create's anchor check
  handles that case.

### 3.4 Create, Rename, Delete, Remove Unreadable

The common flow for all four actions:

1. Check that no structural ticket is pending. The actions are disabled while one is, so this is
   a guard only.
2. Prompt.
3. Call `session.prepare…()`.
4. Call `writer_.submit(write, open_->serial)` and store the ticket in `structuralTicket`.
5. Disable the four actions until the result.

**Create** (Ctrl+N, `+`, disabled while previewing):
- `finishPendingEdit()` first. If that throws, report and stop.
- Pre-check `entries.size() + unreadable >= maximumSnapshots`. If true, show a warning that
  names both counts and points to Delete or Remove Unreadable.
- Prompt with `QInputDialog::getText` titled "New Snapshot", label "Name:", default
  `QLocale().toString(now, QLocale::ShortFormat)`. The name is trimmed; an empty name means the
  default. More than `maximumSnapshotNameBytes` in UTF-8 gives a warning and the prompt again
  with the text kept. Core validation remains the backstop.

**Rename** (F2 on the tree, menu): the same prompt with the current name, titled "Rename
Snapshot".

**Delete** (Del on the tree, menu): a `QMessageBox` titled "Delete Snapshot", text "Delete
snapshot “name”?". The informative text depends on the snapshot:
- with children: "Its N snapshots move to its parent and keep their looks.";
- when it is the working base: "The current look keeps its settings and will be based on its
  parent.";
- always: "This cannot be undone."

The buttons are Delete (destructive) and Cancel (default).

**Remove Unreadable Entries…** (the notice's button, the menu; enabled only if
`unreadableSnapshots() > 0`): a confirmation with the text "Remove N unreadable snapshot
entries from IMG_x.xmp?" and the informative text "They are damaged or were written by another
program or a newer arraw, and cannot be shown. The snapshots listed here are kept. This cannot be
undone."

**Result slot** (`MainWindow::snapshotWriteLanded(SnapshotWriteResult)`), always run on the UI
thread:

1. Clear `structuralTicket` if the result's ticket is the stored one.
2. If `result.stamp` is set, call `filmStrip_->noteOwnWrite(photo, *stamp)` (a new overload
   forwarding to the watcher and `ShotModel::noteSidecarWritten`). This happens even when the
   session is gone, because the write happened.
3. If `!open_`, or `owner != open_->serial`, or the path differs, drop the result. For a
   structural failure, show the status-line message, naming the file.
4. On None, call `finishSnapshotWrite`. For Create, status "Created snapshot “name”." If it was
   not anchored, add: "The look changed while it was written, so the current look is not based
   on it."
5. On Conflict, call `adoptSnapshots(fresh)` and show the status message "The snapshots of %1
   were changed by another program. The list is refreshed; nothing was changed." for 10 s.
6. On LockTimeout, Refused or Error with a structural write, show `QMessageBox::warning` with
   the title "Cannot Create Snapshot", "Cannot Rename Snapshot", "Cannot Delete Snapshot" or
   "Cannot Remove Unreadable Snapshot Entries", and the text path plus `error`.
7. For a thumbnail patch, only the debug log is written, and `thumbnailWritesFailed` is set.
   After that, no further thumbnails are requested for this session, so a stuck lock does not
   make the writer wait 2 s per snapshot.
8. Then refresh the model, the history dock, the actions, the title and the Save state, and
   request thumbnails for IDs that are new (Create) or whose stored key changed.

### 3.5 Thumbnails in the window

- **`FilmStrip`** forwards `beginSnapshots()`, `snapshotGeneration()` and
  `requestSnapshotThumbnail(...)` to its worker. It passes `onSnapshotResult`, which posts to the
  GUI thread and emits `snapshotThumbnailReady(arraw::app::SnapshotThumbnailResult)`.
- **On open** (`showPhoto` and the session rebuild in `sidecarChangedOnDisk`; Discard keeps the
  collection and needs nothing): `snapshotGeneration = beginSnapshots()`. Every snapshot is
  requested with its stored key, and the worker skips the current ones.
- **On `snapshotThumbnailReady`:**
  1. Drop the result unless `generation == snapshotGeneration` and the primary is the session's
     photo (Gate 3 review, finding 5).
  2. Call `model.setRendered(id, image)`.
  3. Then `session.prepareSnapshotThumbnail(id, thumbnail)`, and submit it if not
     `thumbnailWritesFailed`.

### 3.6 External changes and own writes (`MainWindow::sidecarChangedOnDisk`)

The method is reordered so that a change that leaves the session's view of the disk intact
interrupts nothing:

1. Return if no photo is open or the path is another photo's.
2. Read `openDocument(path, log)` first. On failure, show the status message and return,
   touching nothing.
3. If the state, negative and marks on disk equal `session.saved()`:
   - If the collection or the unreadable count differ (thumbnails included), call
     `session.adoptSnapshots(...)`, refresh the model, and request thumbnails for changed
     entries.
   - If `document.savedBase` now equals `session.savedBaseSnapshot()`, **return**: no
     `leaveCropMode`, no `cancelGesture`, no `finishPendingEdit`, no status message. This covers
     own writes that were not noted, a touch, and snapshot-only changes, even with unsaved edits.
4. Otherwise, the active settings or the saved base changed elsewhere, and today's path runs:
   leave the crop mode, cancel the gesture, finish the pending edit. With unsaved edits, adopt the
   collection as well (snapshots are durable whatever the dirty state is), keep the edits and
   show today's message. Without unsaved edits, rebuild the session, close the preview, and start
   a new snapshot generation.

Own writes are noted at the point of the write. The writer notes with the stamp it read (see the
result slot), and Save and marks note as today. The watcher's four-stamp memory and step 3 are
the second line of defence.

### 3.7 Save prompt wording (decision 1)

In `confirmLeavingPhoto`, let `baseOnly` be true when the state and negative equal `saved()` and
the base differs:

- **When `baseOnly`:** text "Save changes to %1?". Informative text: "The adjustments are already
  saved. Only the snapshot the current look is based on has changed (now “%2”, or “none”). Save
  to remember it; Discard to keep the previous one. Snapshots are kept either way."
- **Otherwise:** the text is unchanged. Informative text: "Your adjustments will be lost if you
  do not save them." When the photo has snapshots, add: "Snapshots are already stored and are kept
  either way."

The window title's `[*]` and the Save action follow `hasUnsavedChanges()` as today. The status
line after Create does not mention saving, because the title shows the dirty state.

## 4. Discarding unusable snapshot content (STEP A core, STEP B action)

```cpp
/// @brief Removes from the sidecar the snapshot content this arraw cannot use, keeping every
/// snapshot it lists.
///
/// Under the lock it rereads the sidecar and checks the revision (::arraw::SnapshotRevisionConflict).
/// - It removes every raw entry the reader leaves out: damaged nodes, duplicate IDs, entries
///   past ::arraw::maximumSnapshots. When the list is not a single sequence, it removes every
///   occurrence of the property.
/// - It writes each usable node's parent as the reader repaired it (a missing parent or a cycle
///   edge), and removes a saved base that names no listed snapshot.
/// - It raises the revision, and sets `nextId` above every raw ID seen in all occurrences, so no
///   ID is ever reused.
/// - Usable nodes, and foreign content in them, are patched in place, not rewritten.
/// - A reread must then report nothing damaged, or nothing is written.
/// @return The collection afterwards, or nothing (no write at all) when there was nothing to remove.
/// @throws ::arraw::SnapshotRevisionConflict, ::arraw::SidecarLockTimeout, std::runtime_error.
std::optional<SnapshotUpdate> discardUnreadableSnapshots(const std::filesystem::path& photo,
                                                         std::uint64_t expectedRevision);
```

- **Count** (`SidecarContents`, `OpenDocument`, `SnapshotUpdate`): `unreadableSnapshots` is the
  number of entries whose node is not usable, plus, for a list that is not one sequence, the items
  in all its occurrences (at least 1). This is the same population `unreadableNote()` reports
  today. `snapshotsDamaged` stays as it is.
- **Exception to ADR 056 decision 7:** this is the only path that drops raw snapshot content. It
  is explicit, revision-checked and confirmed by the user. Every other writer still preserves the
  content or refuses the edit.
- **Error text:** the "there is no way to discard them yet" sentence in `unreadableNote()`
  becomes "remove them with Remove Unreadable Entries in the Snapshots panel".

## 5. Steps

Both steps are for Sonnet. Each step ends with both suites green and `just format-check` clean.
In this container, build with `ARRAW_BUILD_PREFIX=container-` and run
`QT_QPA_PLATFORM=offscreen ./build/container-debug/tests/arraw-tests` and
`arraw-widget-tests`. The known flakes are listed in the build note. New cases are tagged
`[snapshots]`, and those taking a second or more also `[slow]`.

### STEP A: core split, writer thread, discard, worker priority

Scope is sections 2 and 4 (core). It covers `include/SnapshotWrites.h`,
`src/core/SnapshotWrites.cpp`, `Sidecar.{h,cpp}` (`setSnapshotThumbnails`,
`discardUnreadableSnapshots`, the counts), `Snapshots.h` (`OpenDocument::unreadableSnapshots`),
`EditSession.{h,cpp}`, `src/app/SnapshotWriter.{h,cpp}`, `ThumbnailWorker.{h,cpp}`,
`FolderWatcher.{h,cpp}`, `CMakeLists.txt` and the tests. There are no MainWindow, FilmStrip or
widget changes.

Acceptance:

1. Every existing test passes unchanged, except tests that assert the old snapshot-before-developed
   order in `test_ThumbnailWorker.cpp`; those are updated to the new order and say so.
2. `test_SessionSnapshots.cpp`:
   - prepare is pure for each kind: the session compares equal before and after, and the file is
     untouched.
   - `prepareCreateSnapshot` throws `std::logic_error` with an open edit.
   - finish anchors Create when the session is unchanged.
   - after an edit, an undo to another position, or a Restore between prepare and finish, Create
     is listed, `baseSnapshot()` is unchanged, and the function returns false.
   - an edit followed by undo back to the same position anchors.
   - a Delete finish remaps the working base, the saved base, the baseline and every history step
     to the parent.
   - `adoptSnapshots` after another process deleted the base remaps to the survivor.
   - `prepareSnapshotThumbnail` returns nothing for an unknown ID and reads no file (it runs with
     the photo file removed).
   - a thumbnail finish changes no history, base, dirty state or revision.
3. `test_Snapshots.cpp`, for `discardUnreadableSnapshots`:
   - (a) with a usable list holding two damaged nodes and one duplicate, they are removed, the
     usable nodes come back equal with a foreign field in one of them kept, the revision is +1,
     `nextId` is above the highest removed ID, and a reread reports `unreadableSnapshots == 0` and
     `snapshotsDamaged == false`;
   - (b) with two `arraw:snapshots` properties, both are removed and `nextId` is above the raw IDs
     of both;
   - (c) a stale revision gives `SnapshotRevisionConflict`, with the file byte-identical;
   - (d) a clean list returns nothing and the file is byte-identical with its mtime unchanged;
   - (e) a dangling parent and a dangling saved base are written repaired;
   - (f) with the lock held, the call times out with the file unchanged (`[slow]`).
4. `test_Snapshots.cpp`, for `setSnapshotThumbnails`: three patches, one with a wrong key, write
   two in one commit, with the revision unchanged and a concurrent rename kept. No match means no
   write.
5. The counts: `readSidecar` and `openDocument` report `unreadableSnapshots` for a damaged node,
   for over-cap entries and for an unusable list, and `SnapshotUpdate.unreadable` after an add to
   a damaged list.
6. `test_SnapshotWriter.cpp` (`arraw-tests`):
   - results come back in submission order with the right owner and ticket;
   - adjacent thumbnail patches for one photo produce one commit, shown by a hook counting core
     calls, and one stamp equal to `fileStamp` afterwards;
   - a conflict result carries `fresh` equal to the file's collection;
   - LockTimeout with a held `QLockFile` (`[slow]`);
   - the destructor with a queued structural write and queued patches (the hook gates the first)
     writes the structural one, drops the patches, and delivers nothing after destruction began;
   - a `writeSidecar` on the test thread while the hook holds the writer inside the lock completes
     after release, with both changes in the file.
7. `test_ThumbnailWorker.cpp`:
   - with an urgent job, embedded jobs, developed visible and off-screen jobs and a snapshot job
     queued, they are served in the order of section 2.4;
   - a snapshot request whose `storedKey` matches produces no result and no render, shown by a
     cache miss counter or the result callback never called.
8. `test_FolderWatcher` (or the existing listing tests): two own stamps noted out of order are
   both suppressed, and a fifth one evicts the oldest.
9. Doxygen on every new declaration, in the noun-form and verb-form briefs CLAUDE.md asks for.
   `EditSession`'s synchronous snapshot methods say that the GUI uses prepare and finish through
   a writer.

### STEP B: GUI and widget tests

Scope is section 3 and the action of section 4. It covers `SnapshotModel.{h,cpp}`,
`HistoryModel.{h,cpp}` (the `wordEdit` extraction), `MainWindow.{h,cpp}`,
`FilmStrip.{h,cpp}`, `CMakeLists.txt`, `docs/keybindings.md` (Ctrl+N, F2 and Del on the tree,
Esc in preview) and the tests.

Acceptance:

1. `test_SnapshotModel.cpp` (`arraw-tests`):
   - the tree shape for roots and nested children in collection order;
   - every role of section 3.2;
   - `ChangeRole` equals `wordEdit` of the edge, and is recomputed after a delete reparents;
   - a thumbnail-only change emits `dataChanged` and no reset, while an ID or parent change
     resets;
   - the placeholder is used when there is no JPEG, and the image is used after `setRendered`.
2. `test_HistoryModel.cpp` passes unchanged after the extraction.
3. `test_MainWindowSnapshots.cpp` (`arraw-widget-tests`). Dialogs are answered by a `QTimer`
   that finds the active modal widget.
   - **Create:** Ctrl+N and the default name lead to a row that appears (`qWaitFor`), the sidecar
     lists it, the title shows `[*]`, and the history length is unchanged.
   - **Save prompt:** after a Create with no edits, closing shows the informative text containing
     "already saved" and "kept either way". With an edit as well, it shows the ordinary text plus
     the snapshots sentence.
   - **Preview:** a click on a row disables the develop dock and the history list, leaves
     `session.photo()`, the history and `hasUnsavedChanges()` unchanged, and makes the rendered
     image differ from before. Esc restores the controls and the image.
   - **Restore:** the banner's Restore adds a "Restore snapshot: name" step, the look equals the
     snapshot's, and the session is dirty.
   - **Rename and Delete:** they change the sidecar and the tree. Deleting the working base
     changes the base marker to the parent, and the confirmation is shown.
   - **Own-write handling** (deferred from Gate 3): with a snapshot lacking a thumbnail and the
     window in the crop mode, wait for the thumbnail to be written and listed, then deliver the
     watcher's event for that sidecar through `FolderWatcher::simulateDirectoryChanged`, or by
     emitting `FilmStrip::sidecarChangedExternally`. The window is still in the crop mode, no
     "changed on disk" status was shown, and the history is unchanged.
   - **External snapshot-only change:** another `addSnapshot` on the file while a mask gesture is
     open, followed by the watcher event, adds the row, keeps the gesture and keeps the history,
     also with unsaved edits.
   - **Damaged list:** a fixture with an unusable node shows the notice. Remove… and Confirm
     leave a sidecar with `unreadableSnapshots == 0` and hide the notice.
   - **Lock held** during Create (`[slow]`): the event loop keeps running (a 100 ms timer fires
     while the write is pending), a warning appears after the timeout, and the tree, the base and
     the history are unchanged.
4. Existing widget tests pass. The `HistoryDock` and `historyList` object names and
   `presetsAndHistoryAction` are kept.
5. The app runs (`just gui`) on a sample: create, preview, restore, rename, delete and quit with a
   pending thumbnail work without a hang. The orchestrator checks this by hand or with the run
   skill.

## 6. Out of scope and still open from the gate reviews

These items are noted and not in this gate unless said otherwise:

- the split A/B view;
- snapshot badges in the film strip;
- "Save snapshot as preset";
- marks-only external changes, which still rebuild a clean session (pre-existing);
- Gate 1 review findings 3, 6 and 9 (decode cost, lock hold measurement, tolerance of a bad
  `created`), to measure or decide later;
- the sidecar size and latency measurements ADR 056 asks for: STEP B's hand check should record
  open, Create and thumbnail-batch times for a sample with 10 brush-heavy snapshots in the ADR's
  measurement list.

## 7. ADR 056 addendum (text)

> **Addendum, 2026-10-25: Gate 4 (GUI).** Plan: [snapshots-gui-plan.md](../ideas/snapshots-gui-plan.md).
>
> 11. **Snapshot sidecar writes from the GUI run on one writer thread.** `EditSession` splits
>     Create, Rename, Delete, thumbnail attachment and discarding into a pure *prepare* on the
>     owning thread, a `SnapshotWrite` value that any thread can carry out with
>     `writeSnapshotChange`, and a *finish* on the owning thread. The synchronous methods remain
>     for the CLI and Python and keep their semantics. Requests run in order, and results return in
>     order. At most one structural request per window is in flight. Adjacent thumbnail patches of
>     one photograph share one lock and one commit. Ordinary Save and marks writes stay
>     synchronous on the UI thread and coordinate through the sidecar lock.
> 12. **Create anchors only an unchanged position.** Finish makes the new snapshot the current
>     step's base only if the session is still at the captured position with the captured look
>     and base, and no edit is open. Otherwise the snapshot is durable and listed, the base is
>     unchanged, and the user is told.
> 13. **A Create with identical pixels leaves the session dirty** (base differs from the saved
>     base), as decision 3 implies. The Save prompt says when only the base changed, and that
>     snapshots are kept either way.
> 14. **Snapshot thumbnails are served after the visible developed thumbnails**, and before the
>     developed thumbnails of shots off screen. Render keys are worked out off the UI thread.
> 15. **Explicit discard of unusable content.** `discardUnreadableSnapshots` removes, under the
>     lock and after a revision check, the entries the reader leaves out (or a list that is not one
>     sequence). It writes repaired parents, clears a dangling saved base, and keeps `nextId` above
>     every raw ID. It is the only path that drops raw snapshot content, and the GUI offers it only
>     after the user confirms. Decision 7 holds for every other writer.
> 16. **Own writes and external changes.** Writers note the sidecar stamp right after their
>     commit, and the watcher remembers the last few own stamps per sidecar. A sidecar event
>     whose active settings, marks and saved base equal the session's ends no gesture: a
>     snapshot-only difference is adopted into the session, with its history kept, whatever the
>     dirty state.
> 17. **Preview.** The window renders a shown photograph built from the snapshot recipe. Editing,
>     undo, Create and Save are disabled until the preview closes. Viewing, marks, navigation and
>     export of the saved settings stay available.

## 8. Open product questions for the user

1. **Export during preview.** Should Export (and the export presets) use the previewed snapshot,
   which would make it the GUI side of Gate 5's `export --snapshot`, or stay on the saved
   settings as this plan has it?
2. **Copy Settings during preview.** Should Copy Settings be allowed during preview, copying the
   previewed look? It is disabled in this plan with the other editing actions.
