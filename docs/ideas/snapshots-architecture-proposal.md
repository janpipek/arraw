# Snapshots: architecture proposal

Status: **accepted architecture**, 2026-10-23; implementation is tracked in
[`todo.md`](../todo.md). The decisions
below are recorded in [ADR 056](../adr/056-snapshots-are-per-photo-looks-with-independent-history-and-sidecar-storage.md).
The limits in this proposal are initial bounds and remain provisional until measured with
brush-heavy snapshots.

## The model

Snapshots are immutable *looks* in a per-photo tree. The settings and negative of a node never
change. Rename, thumbnail regeneration, and reparenting after deletion change its metadata.
Every node stores a complete state; an edge diff is computed when shown. This keeps every node
independently restorable and exportable. A future rebase can create replacement nodes, and a
future merge can extend the ancestry model without making today's nodes depend on parents for
their pixels.

Four small public value types in `include/Snapshots.h`:

```cpp
struct SnapshotId {
    std::uint64_t value = 0;
    auto operator<=>(const SnapshotId&) const = default;
};

struct SnapshotThumbnail {
    std::vector<std::byte> jpeg; // Encoded, at most 256 pixels on either side.
    std::string renderKey;       // Source, snapshot recipe, and thumbnail recipe identity.
};

struct Snapshot {
    SnapshotId id;
    std::optional<SnapshotId> parent;
    std::string name;
    std::chrono::sys_seconds created;
    DevelopState state;
    NegativeSettings negative;
    std::optional<SnapshotThumbnail> thumbnail; // Derived and replaceable.
};

struct SnapshotCollection {
    std::vector<Snapshot> entries;
    std::uint64_t nextId = 1; // Monotonic; deletion never makes an ID reusable.
    std::uint64_t revision = 0; // Structural changes; checked under the sidecar lock.
};
```

Validation requires unique nonzero IDs, a counter above every ID, valid names and recipes,
existing parents, no cycles, bounded count and encoded thumbnail size, and valid JPEG dimensions.
Names need not be unique; operations and CLI address snapshots by ID. Initial limits are 32
snapshots per photo, nonempty names of at most 256 UTF-8 bytes, JPEGs no larger than 256x256 or
64 KiB, and aggregate snapshot content no larger than 128 MiB. These are provisional until
brush-heavy size and latency measurements. IDs and revisions use decimal strings in JSON to avoid
precision loss in JSON number implementations.

`Photo` stays the existing render and export value, unchanged. In particular, a thumbnail
completion or snapshot rename must not make a new `Photo` or change `Photo` equality. A fifth
public value type, `OpenDocument`, carries the extra document metadata when it is needed:

```cpp
struct OpenDocument {
    Photo photo;
    SnapshotCollection snapshots;
    std::optional<SnapshotId> savedBase;
};

OpenDocument openDocument(const std::filesystem::path& path, DiagnosticLog& log);
```

`openDocument()` reads the image metadata and sidecar once; existing `openPhoto()` can use it
and return just `photo` for callers that do not need snapshots. `EditSession` takes the opened
document and owns its collection, working base, saved base, and history. CLI and Python can
inspect `OpenDocument` without creating a GUI session. To preview or export a snapshot, look it
up in the collection and build `photo.with(snapshot.state).with(snapshot.negative)`. This is an
exact recipe copy; it performs no lens rematching or disk write. A separate `SnapshotStore`
class is unnecessary unless sidecar locking grows beyond a few targeted functions.

There is **one new GUI class**, `SnapshotModel : QAbstractItemModel`, for the tree above History.
It indexes nodes by ID and supplies names, thumbnails, parent/child rows, preview and active-base
roles. It computes an edge's settings description through existing `describeChange()`; parent
deletion recomputes it. `MainWindow` can own the actions and preview selection initially. No
branch class, graph repository, merge engine, delta store, or controller hierarchy is needed.

## Session and history

`HistoryStep` and the edit baseline gain `std::optional<SnapshotId> baseSnapshot`. Undo, redo,
and `goTo()` restore this alongside the full state and negative. `EditSession` holds the working
base and saved base separately from its current and saved `Photo`. `hasUnsavedChanges()` compares
all three: rendering state, negative, and base. Thus a change of ancestry with identical pixels
can still need Ctrl+S. `HistoryModel`'s saved marker compares all three too. This extends today's
session model: `begin`, `commit`, `goTo`, save, discard, and equal-state checks must all handle
base changes, including a Restore with no pixel difference.

Proposed `EditSession` methods, with names subject to normal API review:

```cpp
SnapshotId createSnapshot(std::string name);
void restoreSnapshot(SnapshotId id);
void renameSnapshot(SnapshotId id, std::string name);
void deleteSnapshot(SnapshotId id);
```

- **Preview:** find the node in the session's collection and render
  `photo().with(node.state).with(node.negative)` outside the session. No history, base, dirty,
  or disk change. Editing controls are disabled until preview ends.
- **Restore:** append one history step with the snapshot's exact state, negative, and ID as base.
  It must bypass automatic lens rematching. A different base is a step even when the rendered
  settings happen to be equal. Edits after it retain the base.
- **Create:** settle any current gesture, capture its full recipe as a child of the current base,
  write that node immediately, and make the new ID the base of the *current* history position.
  Create adds no step: the next Undo goes to the preceding photo edit and its earlier base;
  Redo returns to the captured position and new base. If there is no earlier position, Undo is
  unavailable. The snapshot remains durable. On write failure, neither the node nor the current
  position's base changes.
- **Rename:** change metadata on disk immediately, then refresh the session collection; no
  history step or change to the working rendering.
- **Delete:** write removal and reparent children to the deleted node's parent, or make them
  roots. Keep every child's settings. Remap the current base, saved base, history steps, and any
  open baseline referencing the deleted ID to that parent or to no base. The tree then records
  ancestry among *retained* snapshots, rather than an immutable audit of original creation.
  Undo does not resurrect the deleted node.
- **Ordinary Save:** write current settings, negative, marks, and base together; preserve the
  latest snapshot collection on disk. It creates no snapshot and retains history.
- **Discard:** restore saved settings, negative, and base. Immediately persisted snapshot changes
  remain.

For example, with A as base: edit, restore B, edit, create C. C's parent is B. The current
history position is now based on C. Undo goes to the preceding Restore-B position; further Undo
crosses Restore back to the earlier A-based work. Redo returns through those positions. Capturing C does
not erase any redo positions, which retain their own earlier bases; a later edit follows the
ordinary rule of dropping redo.

Undo of Create does not delete the durable snapshot. Create anchors the current history entry to
the new ID without adding a step, preserving redo entries. Undo therefore reaches the preceding
photo edit and its prior base; redo returns to the captured position and new base. This avoids a
fallible sidecar write during Undo and avoids a base-only step that would change no pixels.

## Sidecar and concurrent writes

`SidecarContents` gains `SnapshotCollection` and an optional saved working-base ID. XMP stores
one snapshot sequence, its next-ID counter and structural revision, a top-level saved-base ID,
and optional JPEG plus render key in each node. Store each node's state through the existing
versioned state JSON codec, rather than replicating every active setting as nested XMP fields;
the negative has its own existing JSON codec. The exact XMP spelling belongs in the ADR.

Validation for a *write* is strict. Reading a damaged snapshot property is tolerant: an invalid
thumbnail is ignored, an unusable node is reported and skipped, a dangling parent becomes a
root, a cycle edge is cut, a dangling saved base becomes empty, and `nextId` is raised above IDs
read. A damaged snapshot list must not stop an otherwise readable photograph from opening.
Before a later write, preserve unreadable raw content or explicitly refuse the collection edit;
silently replacing the original list with the repaired subset would lose data.

The 128 MiB aggregate bound counts the **serialized UTF-8 XMP snapshot subtree**, including
JSON escaping, base64 thumbnails, and XML markup. Other namespaces and active settings do not
count. `openPhoto()` using `openDocument()` means ordinary opens parse this subtree too; measure
that cost with brush-heavy files before freezing the bound, and add a fast path only if measured
latency warrants it.

Raise `sidecarVersion` from 1 to **2** and write version 2 on **every** successful Arraw sidecar write,
including marks-only, snapshot-only, and thumbnail writes. The user accepts that older builds
will refuse further writes to any such sidecar, including photos without snapshots. Amend ADR
051 to record this deliberate policy; no conditional version selection is needed. Keep reading
version 1 and add frozen version-2 fixtures. Do not change the frozen namespace URI. Unknown
foreign XMP content must still survive edits.

Snapshot operations need targeted writes beside existing `writeSidecar()` and
`writeSidecarMarks()`: add, rename, delete, and attach/replace one thumbnail. They reread the
latest XMP, update only their own fields, and atomically replace it. **Create must not call
`writeSidecar(currentPhoto)`**: that would save unsaved active edits. A newly created node is
durable while the saved active settings and base stay old. Reopening therefore shows those old
settings and base *and* lists the new node. Deleting a node that was the saved base updates that
base to the parent without changing saved settings.

One existing rule needs special care: the version-1 reader treats `arraw:version` alone as
evidence of an explicit neutral develop state. A v2 marks-only or snapshot-only write must not
turn a RAW with no saved develop state into that neutral look. Add an explicit v2
`arraw:activeStatePresent` property and make v2 reading use it to distinguish an absent saved
develop state from an explicit one. Every v2 writer preserves or sets it without writing pending
working edits; the v1 reader keeps its old rule. Fixtures must cover RAW defaults and marks-only
sidecars upgraded through rating, snapshot creation, and thumbnail completion.

If a v2 file's presence property is missing or malformed, warn and infer presence only from
known **top-level active develop** properties: descriptor settings and local adjustments. Do not
infer it from version, snapshots, saved base, marks, or negative settings. A non-active write to
such a damaged file preserves the marker and active fields verbatim; an explicit active Save
writes the complete state and a valid `True`. Newly created or upgraded v2 files always get a
valid marker using the pre-write version's presence rule.

All sidecar writers, including ordinary Save and marks-only writes, take a cross-process
`QLockFile` beside the sidecar around read-modify-write, then atomically replace it with
`QSaveFile`. The lock wait is 2000 ms; failure is reported without changing the file. The
structural collection revision is checked under that lock, so a stale session
cannot erase or rename another process's nodes; `nextId` is allocated there too. Lock behaviour
on network and synced folders needs a test and a documented failure mode. This remains useful
for a single user: the GUI, CLI, Python and background thumbnail job can overlap without a
server or new store class. A targeted thumbnail
write does not advance the structural revision, and must preserve later structural changes. The targeted
thumbnail write checks that its ID still exists and the captured render key still matches;
otherwise it does nothing. Snapshot-only external changes must also be detected by the app's
sidecar-refresh path, which currently compares only active settings, negative, and marks. The
writer must identify its own thumbnail writes so the watcher does not report them as external
edits. Embedded thumbnails make ordinary XMP reads and rewrites larger; measure sidecar size and
latency with several brush-heavy snapshots and bounded JPEGs before fixing the limits.

## Rendering, thumbnails, and other surfaces

Render each thumbnail from its immutable snapshot recipe at low priority, fitted inside
256x256, then encode JPEG. Reuse `ThumbnailCache`'s source/state/negative key ingredients,
but add a thumbnail-recipe revision so engine or sizing changes can invalidate stale images.
Avoid invalidating an otherwise usable embedded thumbnail merely because another machine has
a different lens database; regenerate then only on explicit request or when missing. The
existing `ThumbnailWorker` currently rereads saved sidecar settings; it must accept an explicit
snapshot recipe and ID for these jobs. The UI rejects results for an obsolete open-photo
generation. A missing, invalid, or stale embedded thumbnail shows a placeholder and is
regenerated. Completion writes only that node's JPEG and render key; thumbnail failure never
changes the snapshot's settings, history, or dirty state.

Extend existing surfaces rather than adding parallel command or rendering frameworks:

- `info` shows each snapshot's ID, parent, name, and creation time in text and JSON, plus the
  saved working-base ID. It does not print JPEG bytes.
- `export --snapshot ID` looks up the node in the opened document and copies its state and
  negative into a `Photo` before any explicit export overrides.
  An unknown ID fails clearly; `--no-sidecar` and `--snapshot` conflict. Repeating the command
  with distinct output paths exports several versions of the same input.
- Python binds `SnapshotId`, `Snapshot`, the collection and session operations, and can select
  a snapshot for rendering through the same core path. No GUI-only snapshot semantics.

## Measurements and remaining questions

- Measure sidecar size and read/write latency using brush-heavy states before treating the initial
  count and byte limits as final.
- Test locking on network and synced folders; lock timeout failures leave the original sidecar
  untouched and must be actionable to the caller.
- CLI and Python initially use IDs; unique-name lookup can be considered later without changing
  the persisted model.
