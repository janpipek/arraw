# Snapshots are per-photo looks with independent history and sidecar storage

Status: accepted, 2026-10-23. The architecture and implementation gates are in
[snapshots-architecture-proposal.md](../ideas/snapshots-architecture-proposal.md) and
[`todo.md`](../todo.md).

Snapshots are named, complete develop recipes for one photograph. They support comparing and
returning to looks while ordinary edits continue to have the existing session history. This ADR
chooses a small value model, independent snapshot persistence, and explicit history semantics.

## Decisions

1. **A snapshot is an immutable full recipe.** Each entry has a nonzero `uint64` ID, optional
   parent ID, name, creation time, `DevelopState`, `NegativeSettings`, and an optional replaceable
   thumbnail. The collection has a monotonically increasing next ID and structural revision.
   Parentage records the working base at capture; state differences are derived for display.
   Names need not be unique; operations address IDs.
2. **Photo stays focused on render and export.** `Photo` is unchanged. `OpenDocument` carries a
   `Photo`, the snapshot collection, and the saved working-base ID. `openDocument()` reads the
   image and sidecar once; existing `openPhoto()` remains a compatibility path returning only the
   photo. `EditSession` owns document snapshot metadata and current/saved base IDs.
3. **History records the working base.** Each step stores its optional base snapshot ID beside
   state and negative. Restore adds an undoable step with the exact snapshot recipe and ID; it
   bypasses lens rematching. Create writes a durable child and anchors the current history entry
   to its ID without adding a history step or discarding redo. Undo reaches the prior edit/base;
   redo returns to the captured entry/base. Undo never deletes a snapshot. Rename and delete are
   durable metadata operations and do not become undoable history steps. Delete reparents children
   to the deleted node's parent (or makes them roots) and remaps current, saved, baseline and
   history references to that parent or no base. Ordinary Save writes the active recipe and base,
   and of the marks only those the session itself changed (marks are written at once, so none), so
   a stale session does not put back a rating another process set; it remaps a base another
   process deleted and retries once;
   Discard restores the saved recipe and base, while durable snapshot metadata remains.
4. **Sidecar version 2 carries snapshots.** Read v1 and v2. Every successful Arraw sidecar write
   writes v2, including marks-only and thumbnail writes. This deliberately means older builds
   refuse subsequent writes even to a sidecar with no snapshots. Keep the frozen namespace URI and
   preserve foreign XMP properties. Store each snapshot's state and negative with existing
   versioned JSON codecs. Store IDs and revisions as decimal strings in JSON.
5. **V2 distinguishes absent active state.** `arraw:activeStatePresent` records whether saved
   develop settings are present. V2 readers use it; v1 readers retain their existing rule that
   `arraw:version` implies an explicit state. Marks-only and snapshot-only writes preserve the
   pre-write active-state meaning and must not turn an untouched RAW into an explicit neutral
   state or save pending session edits. If a v2 presence marker is missing or malformed, warn and
   infer presence only from known top-level active develop properties, never the version or
   snapshots. Non-active writes preserve that damaged marker and active fields verbatim; explicit
   active Save repairs it with complete settings and `True`.
6. **All sidecar writers coordinate and replace atomically.** Ordinary Save, marks-only writes,
   structural snapshot operations, and thumbnail writes acquire a sibling `QLockFile` for up to
   2000 ms (a lock older than 5000 ms is stale and taken over), reread the latest XMP, patch only
   the fields they own, and commit through `QSaveFile`.
   Timeout or I/O failure leaves the prior file unchanged and reports a clear error. Structural
   changes check the collection revision under lock and allocate IDs there. A thumbnail patch
   checks that the ID still exists and that the render key the caller expects now (a plain string,
   worked out before the call, so no caller code runs under the lock) is the one the thumbnail
   carries; it does not advance
   the structural revision or overwrite newer collection changes.
7. **Damaged snapshot data does not hide a readable photograph.** Readers report and skip unusable
   nodes, discard invalid thumbnails, cut cycle edges, root entries with missing parents, clear a
   dangling saved base, and raise `nextId` above IDs read. Before modifying a collection that had
   damaged raw snapshot content, the implementation must preserve that raw content or refuse the
   collection edit; it must not silently replace it with the repaired subset.
8. **Initial limits are bounded and provisional.** Up to 32 snapshots per photo; names are
   nonempty and at most 256 UTF-8 bytes; thumbnails are JPEG, at most 256x256 and 64 KiB encoded;
   the serialized UTF-8 XMP snapshot subtree is at most 128 MiB, counting JSON escaping,
   base64 and XML markup but excluding unrelated XMP. Measure brush-heavy sidecars and revise
   limits before treating them as final.
9. **Thumbnails are derived data.** Render at low priority from the immutable snapshot recipe,
   fit inside 256x256, and cache with source, state, negative, and thumbnail-recipe revision in the
   key. Missing, invalid, or stale images show a placeholder and regenerate. A completion patches
   only the matching node's thumbnail and render key. It never changes active recipe, history, or
   dirty state. A different machine's lens database alone does not invalidate a usable embedded
   image.
10. **Surfaces reuse the same recipe model.** `info` lists IDs, parent IDs, names, creation times,
    and saved base in text and JSON, never JPEG bytes. `export --snapshot ID` applies the snapshot
    recipe before explicit export overrides; an unknown ID fails, and `--snapshot` conflicts with
    `--no-sidecar`. Python exposes the same model and operations. The GUI has one `SnapshotModel`
    tree above History; selection previews without changing session state, while Restore is
    explicit. Preview disables editing until closed.

## Verification

Implementation is split into the five gates listed in `docs/todo.md`. Tests must cover value and
graph validation; history capture, restore, undo/redo, deletion remapping and dirty state; frozen
v1/v2 sidecars and RAW active-state presence; unknown foreign XMP preservation; concurrent
structural writes and thumbnail patches; lock timeout; thumbnail size/key validation; UI preview
versus restore; and CLI/Python parity. Add frozen v2 fixtures rather than editing old fixtures.

## Consequences

- A snapshot remains durable across Undo and Discard. Create can leave the saved active recipe
  older than the current working recipe; reopening shows the saved active recipe plus the durable
  snapshot collection.
- Older builds may read v2 data but will refuse all writes to it. This compatibility cost is
  accepted to keep one version policy for every Arraw sidecar property.
- Network and synced-folder lock behavior still needs measurement. Timeout is bounded and never
  falls back to an unlocked write.
- UI polish such as split A/B viewing, multi-snapshot CLI export, snapshot film-strip badges, and
  save-as-preset remain outside this decision.

## Addendum, 2026-10-25: Gate 4 (GUI)

Proposed with [snapshots-gui-plan.md](../ideas/snapshots-gui-plan.md), whose section 7 is the full
text. In short:

11. GUI snapshot sidecar writes run on one writer thread: `EditSession` splits them into a pure
    prepare, a `SnapshotWrite` value carried out by `writeSnapshotChange`, and a finish; the
    synchronous methods remain for the CLI and Python. Requests and results keep their order, one
    structural request per window is in flight, and thumbnail patches of one photograph are
    batched. Ordinary Save and marks writes stay synchronous.
12. Create anchors the new snapshot only if the session is still at the captured position, look
    and base; otherwise the snapshot is listed and the base is unchanged.
13. A Create with identical pixels leaves the session dirty; the Save prompt says when only the
    base changed and that snapshots are kept either way.
14. Snapshot thumbnail jobs run after the visible developed thumbnails; render keys are worked out
    off the UI thread.
15. `discardUnreadableSnapshots` is the one explicit, revision-checked, user-confirmed path that
    drops unusable raw snapshot content; decision 7 holds for every other writer.
16. Writers note the sidecar stamp after their commit; a sidecar event that leaves the session's
    active settings, marks and saved base unchanged ends no gesture, and a snapshot-only change is
    adopted whatever the dirty state.
17. Preview renders a separate shown photograph; editing, undo, Create and Save are disabled until
    it closes.
