# Review: snapshots Gate 1, persistence lens

Reviewer: claude-sonnet-5-5, 2026-10-23. Subject: the uncommitted Gate 1 working tree
(`include/Snapshots.h`, `include/Sidecar.h`, `src/core/Snapshots.cpp`, `src/core/Sidecar.cpp`,
`tests/test_Snapshots.cpp`, `tests/fixtures/sidecars/v2/`, the ADR 051 amendment), read against
ADR 056, `docs/ideas/snapshots-architecture-proposal.md` and the Gate 1 entry of `docs/todo.md`.
Lens: persistence only (locking, atomicity, revisions and ids, `activeStatePresent`, foreign XMP,
tolerant reads versus strict writes, damaged raw content, thumbnail patches, limits, public API).
Static review: nothing was built or run, and no production code was changed. Claims about
behaviour come from reading the code and the tests, not from executing them. Qt behaviour I rely
on (`QLockFile` stale handling, `QDomDocument` serialization) is from memory of the Qt 6 sources.

## Verdict

The persistence core is in good shape and follows the ADR closely. Every writer goes through one
`updateSidecar()` (lock, reread, patch, version and marker stamp, `QSaveFile`), the presence rules
are implemented as written and well tested, structural edits patch nodes in place so unreadable
content survives, and the damaged-list cases refuse cleanly. I found **no defect in the
revision/id allocation or in the `activeStatePresent` logic**. Two issues should be fixed before
Gate 2 builds on this: an unchecked render key can make the whole sidecar invalid XML, and the
saved-base check couples ordinary Save to snapshot existence. The rest is minor.

## Findings, most serious first

### 1. A thumbnail render key is not validated, and can corrupt the whole sidecar (major)

`src/core/Snapshots.cpp:38` checks only that `renderKey` is nonempty. `setThumbnailFields`
(`src/core/Sidecar.cpp:1812`) puts it in a text node verbatim. Names are filtered with
`storableMaskName`, and JSON escapes control characters, but the key is the one free-text field
with no filter, so ADR 056's "strict writes" does not hold for it.

Failing scenario: a caller (the Gate 3 job, or Gate 5 Python, which takes arbitrary strings)
passes a key containing U+0001 or invalid UTF-8. `addSnapshot` or `setSnapshotThumbnail`
succeeds and Qt serializes the control character raw. The next read of the file fails
`parse()` ("not valid XML"), so the active settings, marks and every snapshot are unreadable,
`openDocument` falls back to a bare photo, and every later writer refuses ("is not valid XML"). A
`\r` in a key is milder: XML normalizes it to `\n` on read, so `currentKey(...) != renderKey` and
the thumbnail is silently never accepted. The key also has no length bound, so one key can use
the whole 128 MiB subtree budget (and `setSnapshotThumbnail` does not check that budget, see 8).

Fix, either of: (A) validate the key like a name (`storableMaskName(key) == key`, nonempty, a
modest byte cap such as 256) in `validate(const SnapshotThumbnail&)`, which both the write path
and the tolerant reader already call; (B) store the key base64 or hex encoded. (A) is the smaller
change. Add a test with a control-character key, a `\r` key and an over-long key.

### 2. Ordinary Save fails outright when its saved base was deleted elsewhere (major)

`src/core/Sidecar.cpp:1788-1793` (`writeActive`, called by `writeSidecar(photo, savedBase)`)
throws `std::invalid_argument` when the base ID is not a snapshot in the file at lock time, and
the whole write is abandoned, including the develop settings, negative and marks.

Failing scenario: the GUI session has working and saved base B. A CLI or Python process deletes B
(`deleteSnapshot` moves the file's `savedBase` to B's parent, as ADR 056 item 3 says). The user
presses Ctrl+S. Gate 2 passes the session's saved base B; the writer throws; the photographer
cannot save any edit until the session notices. `std::invalid_argument` also reads as a
programming error to callers, who may not report it as a conflict. ADR 056 item 6 says each writer
"patches only the fields they own" and Save "writes the active recipe and base", but it does not
say Save may fail on a concurrent delete.

Options:
- **A. Dangling base becomes none** with a `SnapshotDamaged` notice (needs a log parameter or a
  return value), and Save proceeds.
- **B. Throw a distinct `SnapshotRevisionConflict`-like exception** and let the session remap and
  retry (the session already remaps deleted bases, ADR 056 item 3). Keeps the semantics strict.
- **C. Take the revision as well** (`writeSidecar(photo, base, expectedRevision)`), so the
  conflict is detected as a conflict rather than as a bad argument.

I would pick B or C: A hides the loss of the user's base, but whichever is chosen, the failure
must not be `invalid_argument`. Add a test (delete the base through another call, then Save).

### 3. Opening a photograph decodes every embedded thumbnail in full (minor, measure)

`validate(const SnapshotThumbnail&)` (`src/core/Snapshots.cpp:47`) calls
`QImage::loadFromData`, which decodes all pixels, and only then compares the dimensions.
`readNode` (`src/core/Sidecar.cpp:1258-1261`) calls it for every node, and `readSidecar` runs on
every `openPhoto`/`openDocument` (GUI open, export queue, CLI export, `info`), plus two more times
per structural write (the pre-read and the after-read in `editSnapshots`, inside the lock).

Failing scenarios:
- A crafted or damaged sidecar holds a 64 KiB JPEG whose header says 8000x8000 (a flat colour
  compresses to a few KiB). Qt 6's default 256 MB allocation limit lets it through or rejects it
  after the allocation attempt; either way up to 32 of them are decoded per open, before the
  256x256 check can reject them.
- A legitimate photograph with 32 snapshots does 32 JPEG decodes, 32 state JSON parses and 32
  negative parses on each open, though most callers (export, `info`, lens lookup) never show a
  thumbnail.

The proposal already says to measure, so this is not a spec breach. Cheap fixes: use
`QImageReader` on a `QBuffer` and read `size()` and `format()` (header only) for validation; keep
a full decode only in the Gate 3 render path. Option: a lazy thumbnail (read the bytes at open,
validate when first shown) and a `readSidecar` flag to skip snapshot parsing for `openPhoto`.

### 4. Unusable nodes use up snapshot slots and cannot be removed (minor)

`addSnapshot` counts `read.list.items.size()` (`src/core/Sidecar.cpp:1912`), which includes nodes
the reader left out, and `nodeOf` (`:1883`) only finds usable nodes, so `deleteSnapshot` throws
"there is no snapshot N" for a damaged one. This follows from "preserve raw content", but nothing
lets the user get rid of it.

Failing scenario: a sidecar with 20 usable and 12 unusable nodes (a bad merge, a hand edit, a
future node shape this build cannot read) refuses `addSnapshot` with "at most 32", although the
photographer sees 20. The message names a limit the user cannot see.

Options: (A) count only usable nodes against 32 but cap total raw items at a higher bound; (B) add
a documented "discard unusable snapshot content" operation (explicit, revision-checked) for the
GUI to offer; (C) leave it and make the message say how many unreadable entries there are. Decide
before Gate 4; C is the minimum.

### 5. A well-formed `snapshots` list is partly read while the report says none is (minor)

`locateList` (`src/core/Sidecar.cpp:1171-1190`) marks the list unusable if the property appears
more than once, but still reads the items of the last occurrence. The report at `:1277` says "so
none is listed", which is false then. `writeActive` (`:1789`) also accepts a saved base against
that partial list.

Failing scenario: two `arraw:snapshots` properties, a bad one first and a good Seq last. Open
shows the good one's snapshots, the notice says none, and edits are refused. The test
(`tests/test_Snapshots.cpp:931`) only orders them the other way round, where the last is the
plain-text one.

Fix: return an empty collection whenever `usable` is false (matches the message and the test), or
change the message. Add the reversed-order case.

### 6. Lock behaviour: stale locks and hold time (minor)

- `SidecarLock` (`src/core/Sidecar.cpp:1586`) never calls `setStaleLockTime`, so Qt's 30 s default
  applies. For a lock left by a crashed process on the same host, Qt breaks it at once (the pid is
  dead). Where the pid cannot be checked (another host on a network share, a container with its
  own pid namespace) every writer fails with "locked by another writer; gave up after 2000 ms" for
  up to 30 s, and the message does not mention a stale lock or the lock file's path. ADR 056 says
  network behaviour is to be measured; at least name the `.lock` path in `SidecarLockTimeout` so
  the photographer can remove it.
- The work done under the lock grows with the snapshot count: reread, a full tolerant read
  (32 JPEG decodes, 32 JSON parses), mutation, a second full read plus `validate`, a full
  serialization for the 128 MiB check, then the write. With brush-heavy snapshots (MBs of JSON),
  a few queued Gate 3 thumbnail patches could keep the lock for longer than a user Save's 2 s
  budget (`QLockFile` is not fair), and Save would then report "locked". Unmeasured, but it is
  the scenario the ADR's measurement item is for. Option: have the thumbnail patch skip the
  second full read (it changes one node) and the subtree measurement.

### 7. Ordinary Save is still last-writer-wins for marks and negative (minor, not a regression)

`patchActive` (`src/core/Sidecar.cpp:1758-1763`) writes a mark whenever the session's value
differs from the file's. After the reread, a Save from a session whose rating is stale writes its
old rating over a newer external one even if the photographer never touched the rating. ADR 056
item 6 says writers "patch only the fields they own"; Save is declared to own the marks, so this
is arguably conformant. Behaviour was the same before this change, and the app's refresh path
(a later gate) is what mitigates it. Noting it because the new lock and reread make it look
protected. Option: the session passes the marks it changed, or Save writes marks only when they
differ from the session's loaded marks.

### 8. Size limits are checked unevenly (minor)

- `setSnapshotThumbnail` (`src/core/Sidecar.cpp:1991-2013`) does not run the
  `maximumSnapshotSubtreeBytes` check that `editSnapshots` runs (`:1855`), so a thumbnail can
  push a subtree that is already at the limit over it.
- The limit is write-side only. A sidecar with a larger list is fully read (`readBytes` plus DOM)
  on every open. Item count is capped at read (first 32), but the DOM already holds it all.
  ADR 056 words the limit as a property of the file, not only of this build's writes; if that is
  the intent, `readSidecar` should refuse or warn above it rather than parse it.
- `addSnapshot` documents (`include/Sidecar.h:312-313`) that exceeding the subtree bound is a
  `std::runtime_error`; the code throws `std::invalid_argument` (`src/core/Sidecar.cpp:1856`).

### 9. Tolerance is inconsistent about which damage hides a snapshot (minor)

A bad `created` makes the node unusable (`src/core/Sidecar.cpp:1223`), so a snapshot with a good
name, state and negative disappears, while a bad thumbnail or bad parent is forgiven. Time is
display metadata; ADR 056 item 7 lists "unusable nodes" without saying which fields qualify.
Suggest: a missing or unreadable time reads as the epoch with a notice, as a bad parent reads as
a root. The same applies to a name that a foreign serializer pretty-printed with surrounding
newlines: `rawText` is untrimmed and `storableMaskName` rejects the newline, so the node is lost.

### 10. Public API and documentation

The API matches the proposal's types and signatures (`SnapshotId`, `SnapshotThumbnail`,
`Snapshot`, `SnapshotCollection`, `OpenDocument`, `openDocument`) and adds `find`, the limits,
`SnapshotUpdate`, the three CRUD functions and `setSnapshotThumbnail`. `Photo` is unchanged
except a comment, as ADR 056 item 2 requires. Differences and slips:

- `setSnapshotThumbnail` takes a `SnapshotKeyFunction` callback instead of "its captured render
  key" (ADR 056 item 6). It works (the stored snapshot's recipe is immutable, so the key can be
  recomputed), but it runs caller code under the cross-process lock, and a key function that is
  slow or throws holds or aborts the write. A plain `expectedKey` parameter compared with the
  stored `thumbnail->renderKey` would not need it, unless the intent is that the key encodes the
  recipe (then say so in the doc).
- `SnapshotRevisionConflict`'s brief (`include/Snapshots.h:104-106`) says it also covers "a
  thumbnail write"; a thumbnail write never checks the revision.
- `SnapshotKeyFunction`'s brief (`include/Sidecar.h:338`) is a verb form on an alias; CLAUDE.md
  asks for noun forms on types.
- `writeSidecarMarks` (`include/Sidecar.h:268-284`): the existing text still says it leaves
  `arraw:version` "as the file has" it and creates a sidecar that "records no develop state"
  without saying it now writes version 2 and a marker. The paragraph added for that sits after
  the `@throws` lines, so Doxygen attaches it to `@throws std::runtime_error`. Move it up.
- `openPhoto` is declared in `include/Photo.h` but defined in `src/core/Snapshots.cpp`. It works
  (the CMake list is updated) but "semantically organised" files would keep it with `Photo`, or
  move the declaration to `Snapshots.h`.
- Snapshot-node JSON is read with the live `log` (`src/core/Sidecar.cpp:1234-1235`), so a
  `SettingMalformed` notice from inside snapshot 7 reads like one from the active settings. The
  notice carries no snapshot id.

### 11. Test gaps (minor)

- "Concurrent" is threads in one process (`tests/test_Snapshots.cpp:765`). The adder threads
  catch `SnapshotRevisionConflict` but not `SidecarLockTimeout`; a 2 s starvation (the lock is not
  fair) would end a thread with an uncaught exception and abort the test run. The ADR's concern is
  cross-process, so add one test that spawns a second process, or at least catch the timeout.
- No test for: a stale lock file left by a dead pid; Save with a saved base deleted meanwhile (2);
  a render key with control characters (1); the reversed duplicate-list order (5); a damaged
  `snapshotRevision`, `snapshotNextId` or `savedBase` value surviving an edit.
- The "failed write" test (`:714`) uses directory permissions and `SKIP`s when they are not
  enforced (root in a container). Fine, but it is the only atomicity test, and it covers the
  lock-creation failure rather than a `commit()` failure after a successful lock.

## Checked and found correct

- **Locking and atomicity:** one lock per `sidecarPath()`, taken before the existence check and
  reread, held to the `commit()`, released by RAII on every throw. Timeout raises a distinct type
  and never falls back to an unlocked write. The no-op thumbnail path leaves the file untouched
  and creates none. `QSaveFile` replaces atomically.
- **Revision and ids:** the revision is read and compared under the lock; `nextId` comes from the
  file inside the lock (raised above every raw id seen, including unusable and over-limit
  nodes) and is written back as `max(next, id + 1)`; ids are never reused; a thumbnail patch does
  not touch the revision or the counter. I found no interleaving of two writers that duplicates an
  id or loses a node.
- **`activeStatePresent`:** the pre-write rule is taken before stamping; v1 files get a marker
  from `declared || keys` (the reader's exact rule, so no drift); a new file gets `False`; a v2
  marker is kept as is by non-active writers, damaged or missing included; an active write
  always writes `True` and the complete state; the damaged-marker reader infers only from known
  active keys and local adjustments. Marks-only sidecars written by the old writer (no
  `arraw:version`) correctly read as no state.
- **Foreign XMP:** snapshots are added as a child of the arraw-holding description, existing
  nodes are patched in place, fields are written as elements so XML attribute normalization
  cannot alter JSON or base64, the nested `rdf:Description` spelling is read, and unknown
  content inside nodes and lists survives.
- **Tolerant reads:** cycles, dangling parents and saved base, duplicate ids, the 32 cap and bad
  thumbnails are all repaired with a notice, and none stops the photograph opening. `highest`
  includes unusable nodes, so a damaged node's id is never reallocated.
- **Fixtures:** `tests/fixtures/sidecars/v2/` is new, v1 fixtures are untouched, and the existing
  tests changed only where `sidecarVersion` moved (the "newer version" cases now use 3).

## Verification

Read in full: `include/Snapshots.h`, `src/core/Snapshots.cpp`, the Gate 1 additions of
`include/Sidecar.h` and `src/core/Sidecar.cpp` (sections "Snapshots", `updateSidecar`,
`patchActive`, `writeActive`, `editSnapshots`, and the four CRUD functions), the old
`writeSidecarFor` (from `HEAD`) for behaviour changes, and the test case list and the lock,
damaged, presence and foreign cases of `tests/test_Snapshots.cpp`. Not run: the build and the tests.
