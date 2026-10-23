# Review: snapshots architecture proposal

Reviewer: claude-sonnet-5-5, 2026-10-23. Subject: `docs/ideas/snapshots-architecture-proposal.md`
(read against `docs/todo.md` Snapshots, ADR 051, `include/Sidecar.h`, `include/Photo.h`,
`include/EditSession.h`). Documentation review only; no code was run or changed. Claims about
current code are from reading headers and greps, not from running tests.

## Verdict

The model is sound: immutable full-state nodes, a base ID in history, targeted sidecar writes,
and no new controller hierarchy all fit the codebase. I would proceed to an ADR, but the
**sidecar concurrency design** and **embedded thumbnails** are under-specified or risky, and a
few edge cases need decisions first. Nothing below blocks the model itself.

## Findings, most serious first

### 1. Concurrency is described as a goal, not a mechanism (high)

"Coordinate per path" and "detect the conflict" cover three different cases:

- **In-process** (Save vs. background thumbnail write): a per-path mutex or queue works.
- **Cross-process** (GUI vs. CLI vs. Python vs. a second GUI): a mutex does nothing. Atomic
  rename prevents torn files, not lost updates between "reread latest XMP" and "replace".
- **`nextId` allocation**: two processes can both read `nextId = 5` and both create ID 5. The
  counter must be allocated under the same exclusion as the write.

Options:
- **A. Advisory lock file** (`photo.xmp.lock`, or `flock` on the sidecar) around read-modify-write.
  Simple and correct on Linux/macOS; weaker on Windows and network or synced folders.
- **B. Optimistic check**: a `revision` integer in the snapshot collection, bumped on each
  collection write; the writer rereads, compares, and retries or fails. Needs no extra file and
  also gives the watcher a cheap "snapshots changed" signal (finding 6). Residual race window
  between the compare and the rename.
- **C. A + B.** Lock for the write, revision for detection and for the app's refresh. Recommended.

Also: a stale in-memory collection. `writeSidecar(Photo)` on ordinary Save must not write
`Photo`'s collection back, or a Save after an external snapshot change erases it. The proposal says
"preserve the latest collection on disk"; make that a stated invariant and test it (Save with a
stale `Photo`, collection changed on disk).

### 2. Embedded thumbnails: cost and churn (high, product decision already agreed)

The todo records embedding as agreed, so this is a risk note and an alternative, not a veto.

- Every write to the sidecar, including a **marks-only write** (a rating keypress in the film
  strip), rereads and rewrites all embedded base64 JPEGs. Rough scale: a 256 px JPEG is roughly
  10-25 KB (unmeasured), +33% in base64; with dozens of snapshots that is on the order of a
  megabyte per rating change. Measure, as the proposal says, but measure *this* path.
- Foreign tools (Lightroom, darktable, sync clients) also read and rewrite the file.
- **Cross-machine churn**: `renderKey` includes "lens data". On another machine with a different
  lensfun version every thumbnail looks stale, regenerates, and rewrites the sidecar; a synced
  folder then conflicts. A snapshot's recipe is immutable, so staleness should depend only on
  the thumbnail-recipe revision (engine/sizing) and maybe the source file identity. I would drop
  lens data from the key, or treat a mismatch as "usable, regenerate only if missing".
- Derived data lives in the user's portable, hand-inspectable document, which also creates most
  of the extra write paths (targeted thumbnail write, key check, watcher self-write detection).

Options:
- **A. Keep embedded** (as agreed) with the simplified key above and measured bounds.
- **B. Thumbnails in the app cache** keyed by (source identity, snapshot ID, recipe hash,
  thumbnail revision), reusing `ThumbnailCache`. Removes the thumbnail write path, the
  watcher-self-write problem, and sidecar bloat. Cost: thumbnails are not portable between
  machines and are regenerated after a cache clear.
- **C. Hybrid**: cache first, embed only on an explicit request. Probably more than v1 needs.

### 3. Version 2 for every sidecar (medium)

The proposal admits the bump is not technically needed: ADR 051 says an old arraw keeps unknown
keys on write. The cost: an old arraw can no longer write marks (ratings) to *any* sidecar a new
build has saved, snapshots or not. This is the user's request, so I would not override it, but
the choice affects migration. Alternative: **write version 2 only when the sidecar contains
snapshots** (version = what the content needs), which keeps the protection where it matters. That
bends ADR 051's "one version" wording slightly, so the ADR amendment should say so either way.
Either way, state when version 2 is first written.

### 4. Reading must be more tolerant than validation (medium)

"Validation requires ... no cycles, existing parents ..." is right for writing. For *reading* a
file another process or a foreign tool may have damaged, a bad snapshot must never stop the photo
opening. Specify the repair policy in the ADR: drop a node with an invalid recipe (Notice), cut
a cycle or dangling parent to root, drop a dangling saved base, ignore a bad thumbnail, clamp
`nextId` above the largest ID. Also test hand-edited and truncated XMP.

### 5. Node storage format (medium)

"Nested snapshot fields must never be mistaken for the active photo's settings" is a real
hazard with per-key XMP structures. Options:
- **A. XMP structure per node** mirroring today's keys: inspectable, but duplicates the
  serialisation and risks the confusion noted.
- **B. One property per node holding the existing state JSON** (`settingsJsonVersion`) plus the
  negative: reuses the versioned serialiser, keeps nodes opaque to the flat reader, smaller
  diff. Less human-readable. Recommended unless a foreign tool should read the nodes.

### 6. Refresh path and equality (medium)

- The app's sidecar-refresh compares only settings, negative, and marks. The `revision` from
  finding 1 (or a collection hash) is the simplest way to extend it.
- `Photo` equality: deep comparison of snapshot collections whenever pointers differ is costly
  and easy to get subtly wrong. Compare base ID and collection `revision` instead, and keep
  thumbnails out entirely. See "Challenge 1" below: I now recommend
  *against* `Photo` owning the collection.

### 7. Create makes the session dirty (medium, UX)

Because the new base is a history step and Save is what persists it, Create Snapshot alone makes
`hasUnsavedChanges()` true: a close prompt and a dirty marker for an action that has already
written to disk. Discard then reverts the base to the old one while the new node stays, which can
look like the snapshot "lost" its link. This matches the agreed behaviour, but the UI should
explain it (the history label already helps). Alternative: persist the base too on Create when
the rest of the session is clean (nothing else unsaved); less surprising, but it contradicts the
"saved base stays old" rule in the todo, so it needs the user's decision.

### 8. Delete edge cases (medium)

- Remapping the base across history can leave **consecutive identical steps**; decide whether to
  coalesce or keep them.
- A "Based on snapshot X" label must resolve the name at display time and degrade after X is
  deleted ("deleted snapshot"), or the label goes stale.
- Redo stack: the remap applies to it too; test explicitly.
- Delete of the *previewed* snapshot: end the preview first.

### 9. Create details to specify (low)

- Create with no change since the base: allow (an explicit duplicate) or refuse? Say which.
- Name defaults and the bounds for length and count (design question 3). From the sizes above I
  would start at about 32 nodes and 24 KB per thumbnail, then adjust after measurement.
- Create while a render or an open gesture is pending: "settle any gesture" is covered; cover a
  failed write mid-settle.

### 10. Smaller points

- **ID range**: `uint64` in XMP/JSON/Python. IDs stay small but JSON consumers lose precision
  above 2^53; either bound `nextId` at validation or document it.
- **Naming**: `ExportQueue.h` and `MainWindow.h` already use the word "snapshot" in their own
  sense (per grep; I did not read them). Check for a collision before the public name is fixed.
- **Preview** goes through the same render path: confirm the histogram, clipping overlay, and
  crop overlay either follow the previewed snapshot or are explicitly disabled.
- **`SnapshotModel`**: fine for a tree of dozens; no objection to `QAbstractItemModel`, but
  `QStandardItemModel` is enough if the roles stay this small.
- **CLI/Python create**: the proposal lists `info` and `export --snapshot` but no way to create
  or delete from the CLI. If that is intentional, say so.
- **Test plan** is missing from the proposal; at minimum: frozen v2 fixtures, the two-process
  write race, stale-`Photo` Save, base-only dirty/undo/redo, delete remap, thumbnail staleness.

## Challenges to the two requested choices

### Challenge 1: `Photo` should not own the collection (I changed my mind from the first pass)

- The base ID does not affect pixels, and `withSnapshot(id)` needs no collection:
  it is `photo.with(snapshot.state).with(snapshot.negative)` using the existing `Photo::with`.
  The only reason for `Photo` to hold the collection is to find the snapshot by ID.
- `Photo` is the render/worker/export value. Putting mutable document data in it means a rename
  or a finished thumbnail produces a "new `Photo`", and every site that compares or keys on `Photo`
  must be audited to ignore it. The proposal already needs a "narrower rendering identity" plus a
  custom `operator==`; that is the smell. A missed site means spurious re-renders or, worse, a
  missed one.
- Stale copies: `Photo`s held by the export queue or workers carry old collections silently.

Options:
- **A. `EditSession` owns collection and base** (it already owns history, baseline, saved state).
  A new `openDocument()` returns `{Photo, SnapshotCollection, savedBase}`; `openPhoto()` stays
  for callers that do not care. CLI/Python: open, look up, `photo.with(...)`. `Photo` and its
  equality are untouched. Recommended.
- **B. Keep as proposed**, with revision-based equality (finding 6). Fewer signatures change but
  carries the risks above.
- **C. Separate `SnapshotStore` object** (read/write, lock, revision, thumbnails) that the session
  and CLI both use. Worth it if finding 1's locking grows; otherwise A is enough.

### Challenge 2: Undo of Create leaving the node is the weakest part of the design

- Ctrl+Z after Create visibly does nothing (pixels equal; the node stays), spends an undo press,
  and a mistaken Create cannot be undone at all; only Delete (itself not undoable) removes it.
  That breaks the user's expectation of what Undo means for the action it follows.
- The dirty/Discard consequences (finding 7) follow from the same choice.
- Restore needs the base in history; Create is the one that does not have to.

Options:
- **A. Undo removes the node, Redo recreates it** with the same ID (the counter is monotonic, so
  the ID is reserved by the step). Simple mental model. Cost: Undo/Redo write the sidecar, so they
  can fail or conflict (report and stay put); a node deleted by Undo is gone if another process
  already referenced it (rare; check and refuse).
- **B. As proposed**, but label the step clearly and add "Delete snapshot" to the status message
  offered on Create. Keeps writes out of Undo, at the UX cost above.
- **C. Create is not a history step**: the base switches immediately, outside history, and earlier
  steps keep their old base. Needs a rule for what the base is after undoing past Create; I think
  that rule is harder to explain than A or B.

I recommend A. If writes in Undo are unacceptable, B with a prominent label, not C.

### On the version bump

No further objection beyond finding 3. The explicit cost statement is the right thing; the open
choice remains version-always-2 versus 2-only-with-snapshots.

## Answers to the design questions

1. **Collection in `Photo`**: no; see Challenge 1.
2. **Undo of Create keeping the node**: not clear enough; see Challenge 2.
3. **Bounds**: start near 32 snapshots and 24 KB per thumbnail; confirm by measuring the
   marks-only rewrite (finding 2).
4. **Names in the CLI**: IDs only at first; names are not unique, and a later `--snapshot-name`
   that fails on ambiguity is additive.
