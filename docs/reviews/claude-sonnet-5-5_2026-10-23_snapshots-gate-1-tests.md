# Review: snapshots Gate 1, tests lens (uncommitted working tree)

Reviewer: Claude Sonnet 5.5, 2026-10-23. Scope: the uncommitted changes implementing Gate 1 of
[ADR 056](../adr/056-snapshots-are-per-photo-looks-with-independent-history-and-sidecar-storage.md)
(model and persistence), read against the ADR's Verification list and the Gate 1 entry of
[`todo.md`](../todo.md). Lens: do the tests prove what the ADR says they must. Files read:
`tests/test_Snapshots.cpp` (973 lines), the edits to `tests/test_Sidecar.cpp` and
`tests/test_LocalAdjustmentsSidecar.cpp`, `tests/fixtures/sidecars/v2/*`, and the implementation in
`src/core/Sidecar.cpp` and `src/core/Snapshots.cpp` as far as needed to see what an assertion can
and cannot catch. I did not build or run anything (another agent owns the build dir), so every
"would pass" below is by reading, not by mutation.

**Counts:** major 6, medium 6, minor 6.

Summary: the happy paths, the validation limits at 32 / 256 bytes / 256 px, RAW defaults from a
sidecar-less start, frozen v2 reading, and the "keep unusable raw nodes verbatim" rule are
well covered. The weak spots are the properties that need a race or a failing disk to show:
"reread under the lock", atomic replacement, and concurrent writers are tested in ways that
a lock-less or read-before-lock implementation would likely still pass. The 128 MiB limit has no
test at all. The lock-timeout test costs 8 s in the default suite.

Existing-test edits: benign. `test_Sidecar.cpp:176-183` moves from `version="1"` to `"2"` and adds
`activeStatePresent` (stronger). The three `"2"` to `"3"` changes
(`test_Sidecar.cpp:546,677`, `test_LocalAdjustmentsSidecar.cpp:583`) are needed because 2 is no
longer newer. The one real narrowing is M5.

---

## Major

### T1. Nothing proves a writer rereads the file after taking the lock

`tests/test_Snapshots.cpp:361-381` (revision), `:420-462` (Save keeps the list), `:765-830`
(concurrent). Every revision-conflict and "Save keeps the snapshots" test is sequential: the
file is fully written before the next writer starts. An implementation that parsed the sidecar
first and then took the lock (`updateSidecar`, `src/core/Sidecar.cpp:1617` is the lock, the read
follows it) passes all of them. ADR 056 section 6 ("acquire ... reread the latest XMP ... checks
the revision under lock") is the heart of the lost-update protection and is only checked by the
probabilistic thread test.

Failing scenario: swap lines so `parse(readBytes(path))` runs before `SidecarLock`. Process A reads
at revision 3, B adds and commits (revision 4), A gets the lock and commits revision 4 built on
revision 3 content, B's snapshot is gone. No test fails deterministically.

Fix: a deterministic test. Hold a `QLockFile` on `IMG_1.xmp.lock`; start `addSnapshot(.., 0, ..)`
on a thread; wait until it is blocked (e.g. poll a flag set before the call plus a short sleep, or
just do the next step blindly); rewrite the sidecar by hand to revision 1 (or call a writer
that bypasses the lock via `spit`); release. Expect `SnapshotRevisionConflict{0, 1}`. The same for
`writeSidecar`: hold the lock, `spit` a sidecar with two snapshots, release, and require the Save
kept both.

### T2. The 128 MiB subtree limit is not tested at all

Limit: `include/Snapshots.h:36`, enforced only at `src/core/Sidecar.cpp:1855`. Grep of
`tests/` for `maximumSnapshotSubtreeBytes` finds nothing. ADR 056 section 8 and the Gate 1 entry of `todo.md` repeat it
("counting JSON escaping, base64 and XML markup but excluding unrelated XMP").

Failing scenarios, none caught: changing `>` to `>=`; measuring the whole document instead of the
list element; measuring `QString` UTF-16 size instead of UTF-8 (the counting rule differs for
non-ASCII names and the escaped JSON); dropping the check entirely. With 32 snapshots, 64 KiB
thumbnails and small states the legitimate content is a few MB, so no existing test gets near it.

Fix: a seam is needed, because building 128 MiB through the public API is not realistic (state JSON
is bounded by `maximumLocalAdjustments` and brush data). Options: (a) make the limit a parameter of
an internal `editSnapshots` and test it with a small value, including exactly-at and one-over, and
foreign XMP outside the list not counting; (b) expose `SnapshotLimits` in a private header
(`src/core`) that tests can lower; (c) fabricate a big list by `spit` and test only the refusal
(no add succeeds on a 129 MiB list), which is slow but needs no seam. I prefer (a).

### T3. The atomicity test fails at the lock, not at the replacement

`tests/test_Snapshots.cpp:714-737`. Making the directory unwritable stops `QLockFile::tryLock`
from creating `IMG_1.xmp.lock` (`Sidecar.cpp:1594-1597`, "no permission to make its lock file"),
so the writers never reach `QSaveFile` (`Sidecar.cpp:1674-1687`). What is proven is "no lock, no
write", not "a failed write leaves the old file". The `Restore` guard and the probe are careful,
but:

- It is skipped (`SKIP`, `:730`) when the process can write anyway, e.g. root in a container or
  Windows, where `fs::permissions` does not stop creation. On such a runner the ADR's atomicity
  item is silently unproven and the suite is green.
- It never checks for leftovers (`*.lock`, `QSaveFile` temp) after the failure.
- A short write or failed `commit()` is never exercised.

The in-memory half of atomicity is covered by the "refuses what is invalid" test
(`:329-359`, byte-for-byte comparison), which is good, but that throws before the file is opened.

Fix: assert the directory listing is unchanged after the failure; add a case that fails after
the lock: make the sidecar path itself unwritable (pre-create `IMG_1.xmp` read-only inside a writable
directory: the lock is made, `QSaveFile::commit` replaces by rename and may still succeed, so this
needs care on Linux) or, more robustly, give `updateSidecar` a test seam for the byte sink. Print a
`WARN` or fail on `SKIP` in CI so a skip is noticed.

### T4. Concurrent test: unsafe in threads, can hang, can terminate, and does not need the lock to pass

`tests/test_Snapshots.cpp:765-830`.

- `thumbnailOf` calls `REQUIRE(image.save(...))` (`:93`) and the worker at `:801` calls it from a
  non-main thread. Catch2 assertions are not thread-safe unless
  `CATCH_CONFIG_EXPERIMENTAL_THREAD_SAFE_ASSERTIONS` is on (the fallback pin is v3.13.0,
  `tests/CMakeLists.txt:8`; a system install may be older). A failure there is UB, not a failure.
- `readSidecar(bench.photo)->snapshots` (`:783`) dereferences an unchecked optional, and the
  read is lock-free against another thread's `QSaveFile` commit. On Windows (non-atomic rename
  semantics for open files) this is the likeliest flake.
- The adders catch only `SnapshotRevisionConflict` (`:788`). A `SidecarLockTimeout` (2000 ms under
  four contenders on a loaded debug runner) or any `runtime_error` escapes the thread lambda and
  calls `std::terminate`, killing the whole `arraw-tests` binary rather than failing one case. The
  thumbnail thread catches the timeout but not others, and both loops are `for (;;)`, so a
  persistent failure hangs the run.
- No assertion that contention happened. If the OS runs the threads one after another every
  adder succeeds first time and the test degrades to a sequential one. Without the lock the
  expected result (15 revisions, 15 ids) still holds some of the time.
- Threads in one process only; ADR says "concurrent structural writes". Lock behavior across
  processes (the `QLockFile` pid/host logic) is not exercised. Save and marks writers are not in
  the mix, though they take the same lock (ADR section 6).

Fix: collect results in `std::vector<std::exception_ptr>` / atomics and assert on the main thread;
build the thumbnail JPEGs before starting threads; bound the retries (e.g. 200 attempts, then
record a failure); catch `SidecarLockTimeout` in the adders too; count conflicts (not asserting
> 0, but logging it); add one `writeSidecar` and one `writeSidecarMarks` thread. For cross-process
coverage, consider a small helper executable or `QProcess` re-invoking `arraw-tests` with a
`[.worker]` case.

### T5. The lock-timeout test costs 8 s in the fast suite and measures one wait

`tests/test_Snapshots.cpp:739-763`. Four writers each wait the full 2000 ms
(`addSnapshot`, `writeSidecar`, `writeSidecarMarks`, `setSnapshotThumbnail`), at least 8 s of
wall time. The case is tagged `[snapshots][lock]` only, while the project's convention for a second
or more is `[slow]` (`tests/CMakeLists.txt:195-199`; `just test-fast` skips it). The timeout constant
is `inline constexpr int sidecarLockTimeoutMilliseconds = 2000` so there is no way to shorten it,
unlike the presets store test which uses 100 ms (`test_ExportPresetStore.cpp:315`).

Also:

- Only the first call is timed (`:748-752`). The window `>= 1800 ms` and `< 10 s` is the only
  timing check; the others could wait 9 s or return at once and pass. Under load the `< 10 s`
  upper bound of the first call is the one assertion that can flake.
- The marks writer is asserted by message (`ContainsSubstring("IMG_1.xmp")`, `:754`), not by
  `SidecarLockTimeout`. A permission error also names the file, so a wrong error class passes.
  `writeSidecar(photo, base)` (the two-argument overload) is not covered at all.
- No test of a stale lock (a crashed writer's `.lock` must not block forever);
  `test_ExportPresetStore.cpp:332-344` has a Linux model to copy.

Fix: tag the case `[slow]`, or make the timeout injectable (parameter on the internal
`SidecarLock`, or a private `setSidecarLockTimeoutForTests`) and use 100 ms; assert the exact type
for every writer; test each writer in a SECTION so a failure names it; add the stale-lock case.

### T6. "nextId is raised above every id read" is asserted with a case that cannot tell the difference

`tests/test_Snapshots.cpp:857-880`. The stored counter is 2 and the highest usable id is 9, so
`nextId == 10` (`:878`) is satisfied by an implementation that only looks at usable nodes. The
implementation does track unusable ids (`Sidecar.cpp:1294`, `rawId` of a node that failed), which is
the point of ADR 056 section 7 ("raise `nextId` above IDs read"), but the test would not notice
if that were removed.

Failing scenario that matters: an unusable node `id=12` (bad state JSON) stays in the file
verbatim (the edit path keeps it). If `nextId` is not above 12, the 3rd add after a repair reaches id 12
and the file now holds two nodes with id 12, one unreadable; after the user fixes the JSON by
hand the reader reports a duplicate and drops one of them.

Fix: add an unusable node with an id above all usable ones (and above the stored counter), assert
`nextId == 13`, and in the "edit keeps raw content" SECTION assert the added id is 13.
Related: `CHECK(count(log, Notice::SnapshotDamaged) >= 8)` (`:881`) is a loose floor; the file
contains at least nine distinct defects, so an implementation missing one report passes. Assert
the exact count, or at least each defect's id/position in `log.entries()[i].values`.

---

## Medium

### M1. Read-side damage is mostly untested beyond the one big fixture

`tests/test_Snapshots.cpp:832-882`. The node list covers: bad state JSON, duplicate id, missing
parent, a cycle, no id, empty name, empty thumbnail key, bad base64. Not covered, each a separate
branch in `Sidecar.cpp:1190-1320` (`readNode`, `readSnapshotsIn`):

- more than 32 nodes in a file (`Sidecar.cpp:1287`, "the list is past 32"); and what a following
  `addSnapshot`/`deleteSnapshot` does with 33+ nodes (`read.list.items.size() >= 32` counts unusable
  nodes too).
- name of 257 bytes, invalid UTF-8, or control characters on read (only the empty name is read).
- `created` missing, unparsable, before 1970 or after 9999 on read.
- thumbnail that is valid base64 but oversized (> 64 KiB), larger than 256 px, or not a JPEG: the
  three outcomes ADR 056 section 7 names ("discard invalid thumbnails") are tested only for
  an empty key and bad base64. `validate(SnapshotThumbnail)` is tested on the write side only.
- `parent` equal to own id, zero, or non-numeric.
- `savedBase` of 0 or non-numeric; `snapshotRevision` malformed.
- id `18446744073709551615` (`Sidecar.cpp:1366-1368` saturates, so `validate(collection)` fails
  `nextId > id` for what the reader returned; `Sidecar.cpp:1850` computes `id + 1` which wraps to 0).
- `SidecarContents::snapshotsDamaged` (`include/Sidecar.h`) is only asserted true in
  `:916`; no case checks it is false when only a thumbnail was dropped, which its doc comment
  promises.

Fix: a table-driven test, one defective node per row, each asserting that the node is left out,
the other nodes read, exactly one `SnapshotDamaged` notice with the right values, and that
`snapshotsDamaged` has the documented value.

### M2. The "rename in between is kept" assertion is vacuous

`tests/test_Snapshots.cpp:411-417`. The comment says a rename in between is kept, but
`renameSnapshot(bench.photo, 2, first, "First")` renames to the same name, so nothing could
be lost. `keyOf` (`:113`) is the node's name, so the intended scenario, a thumbnail
computed for the old key arrives after the snapshot's key changed, is never produced either:
the only non-matching key is the literal `"key-Old"` (`:403`) against a node that never had it.
ADR 056 section 6 lists "captured render key still matches" and "does not overwrite newer
collection changes".

Failing scenario: a patch that overwrites the whole node (name included) from a stale copy would
revert a rename and the test would still pass, because the name is unchanged.

Fix: rename `first` to "First 2", then patch with the old key (`key-First`): expect `false`,
file unchanged. And rename `second` meanwhile and patch `first` with a correct key: expect `true`
and `second`'s new name and the revision unchanged.

### M3. Frozen v2 fixtures are two files, and the risky shapes are inline

`tests/fixtures/sidecars/v2/` holds `complete.xmp` and `marks-only.xmp`. ADR 056 asks for "frozen
v1/v2 sidecars and RAW active-state presence" and "frozen v2 fixtures rather than editing old
fixtures". Missing as files:

- a v2 sidecar with `activeStatePresent="False"` **and** snapshots (the snapshot-only RAW, the
  ADR's headline risk);
- a v2 sidecar with damaged nodes (what the big inline test builds in `v2Sidecar(...)` strings,
  `:118-124`, with state JSON produced by today's `stateToJson`, so a later change to the encoder
  silently changes the "frozen" input);
- a v1 sidecar of marks alone (the test at `:490-499` uses an unversioned string and is named
  "a version 1 sidecar"; there is no v1 marks-only file in `sidecars/v1/`).

Further, no golden check of what the writer emits. `addSnapshot` then `openDocument` is a symmetric
round trip (`:285-327`); a writer and reader that both misspell a property (`snapshotRevision`) agree
with each other. Only `complete.xmp` anchors the property names, and only for reading.

Fix: add the three fixtures; add one test that adds a snapshot to an empty directory and compares
the parsed XML structure (element and attribute names, not whitespace) with a frozen expected file.

### M4. "RAW defaults across marks, snapshot, thumbnail upgrades" is covered for one path

`tests/test_Snapshots.cpp:464-529`. The marks, then snapshot, then thumbnail chain is proven from a
fresh directory (`:473-489`). The same must hold, and is not tested, for:

- `renameSnapshot` and `deleteSnapshot` on a RAW-default sidecar (no `requireDefaults()` after them,
  although they are the writers that change `snapshotRevision` and may leave the marker alone);
- a pre-existing unversioned marks-only file followed by `setSnapshotThumbnail`;
- the v1-with-state branch (`:506-514`) after a thumbnail, rename or delete (only `addSnapshot`);
- the v2 marks-only frozen fixture being written (it is only read, `:639-648`).

Also `rawDefault` (`:467`) is never asserted to differ from `DevelopState{}`; it does
(`rawDefaultColorNoiseReduction`) but nothing in the test says so, so if the fixture changed
to a non-RAW the `state() == rawDefault` check would become vacuous while the
`CHECK_FALSE(bench.read().state)` half still holds. Add `REQUIRE(rawDefault != DevelopState{})`.

### M5. The `test_Sidecar` loop now skips a frozen fixture silently

`tests/test_Sidecar.cpp:1007-1009` skips `marks-only.xmp` in the "every stored version is read and
rewritten" loop, by file name. The reason (it records no state, so
`requireVersionOneContents` would fail) is right, but the effect is that the v2 marks-only
fixture is never put through `writeSidecarMarks` / `writeSidecar` anywhere. Any future fixture
named `marks-only.xmp` in `v3/` is skipped too. Prefer a per-fixture manifest or a separate loop
in `test_Snapshots.cpp` that reads and rewrites every `marks-only.xmp` and asserts state stays
absent after a marks write.

### M6. Foreign XMP is tested around the list, not through it

`tests/test_Snapshots.cpp:685-712` starts from `sidecar-foreign.xmp`, which has no snapshots,
and compares `others`, `creatorTool`, `negative`, `state`. `:628-637` checks `acme:Owner` survives
`Save` and a marks write on `complete.xmp`, but not any of add / rename / delete / thumbnail on that
file. Not covered:

- foreign or future `arraw:` fields **inside** a snapshot node (e.g. a v2.1 `arraw:color`)
  surviving a rename, thumbnail patch and delete of a sibling (ADR section 6: "patch only the
  fields they own");
- a list that lives in a second `rdf:Description` (`fieldHolder` and `locateList` search all of
  them; `addSnapshot` appends to `home`);
- foreign elements in a node written by another XMP tool as a nested `rdf:Description` (the
  `fieldHolder` fallback at `Sidecar.cpp:1103-1122` has no test at all).

Fix: use `v2/complete.xmp` with an added foreign property inside one node, or a new frozen fixture,
and run the full add/rename/thumbnail/delete sequence over it.

---

## Minor

### N1. Validation boundaries: only the failing side is tested

`tests/test_Snapshots.cpp:185-190` rejects `-1` and `253402300800` but never accepts `0` and
`253402300799`, nor round-trips them through the file (`spellTime` / `timeIn`,
`Sidecar.cpp:1051-1067`). The limit exists because the ISO spelling cannot hold more; a round trip
at the maximum is the test that justifies it. Thumbnails: `:198-199` accepts 256x171 and 1x1 but not
256x256 itself, and there is no test at exactly `maximumSnapshotThumbnailBytes` (the `heavy` case at
`:226-228` is zero bytes, caught by the size check before any decode). A JPEG bomb (tiny file,
declared 30000x30000) is not tried, though `validate` decodes the image to learn its size
(`Snapshots.cpp:51-55`); Qt's allocation limit probably rejects it, which is exactly what a test
should pin. The render key has no upper bound and no test (`validate` only requires non-empty).

### N2. Names: whitespace and escaping edge cases

`:313,949` cover `&`, `<`, quotes and an emoji. Leading or trailing spaces, a name of only spaces,
a tab, `\r`, and U+2028 are not round-tripped; `rawText` returns the unnormalised text on read but
`fieldOf(...).text` (used for ids) is trimmed, and the XML parser may normalise `\r`. One
parameterised round trip would close it.

### N3. Test titles overclaim

`"Snapshot writes do not save pending edits, and Save keeps the snapshots"` (`:420`) cannot test the
first half: `addSnapshot` takes a path, not a `Photo`, so there are no pending edits for it to save.
The test does prove the sidecar's active recipe is left alone, which is worth keeping under that
name. `"from a version 1 sidecar of marks alone"` (`:490`) builds an unversioned file via the helper
`v2Sidecar` (misleading name for a v1-shaped file).

### N4. Missing `Notice::SnapshotDamaged` message and name tests

`src/core/Diagnostics.cpp` (new case), `src/app/DebugLog.cpp` and `src/cli/StreamDiagnostics.cpp`
add the notice. No test renders its message (`test_Diagnostics.cpp` covers other notices), and the
value order (position, id, why) is documented in `include/Diagnostics.h` but unchecked. A wrong
order would print "snapshot Bad state (id ...)" with nobody noticing.

### N5. Includes of the new test file

`tests/test_Snapshots.cpp` uses `std::memcpy` (`:96,222`) with no `<cstring>`, `std::optional`,
`std::string_view`, `std::uint64_t`, `std::runtime_error` and `QString` with no direct include.
It compiles through transitive includes; CLAUDE.md asks for the standard-library group to be
complete. `.clang-format` will not add them.

### N6. Unused and unchecked assertion return values

`:913` `CHECK_NOTHROW(openDocument(...))` discards a `[[nodiscard]]` value (warning in some
builds); `:693,696` ignore `setSnapshotThumbnail`'s bool in the foreign test, so the
thumbnail write that test claims to cover could return false (key mismatch) and the foreign
properties would be compared after a write that never happened. Use `REQUIRE(...)`.

---

## Verification list against the tests

| ADR 056 Verification item | Covered by | Verdict |
|---|---|---|
| Value and graph validation | `:157-273` | Good; see N1 for boundaries |
| Frozen v1/v2 sidecars | `:601-668` and `test_Sidecar.cpp` loop | Reading good; fixtures thin (M3, M5) |
| RAW active-state presence | `:464-599` | Good for the main chain; M4 for the rest |
| Foreign XMP preservation | `:685-712` | Around, not through, the list (M6) |
| Atomic replacement | `:329-359`, `:714-737` | In-memory yes; disk failure not really (T3) |
| Revision conflicts | `:361-381` | Sequential only (T1) |
| Lock timeout | `:739-763` | Works, 8 s, one timing (T5) |
| Concurrent writes and thumbnails | `:765-830` | Fragile, probabilistic (T4) |
| Thumbnail size and key validation | `:197-229`, `:383-418` | Write side yes; read side no (M1, M2) |
| Damaged data | `:832-945` | Good skeleton, loose assertions (T6, M1) |
| 128 MiB / 32 / 256-byte limits | 32 and 256 yes | 128 MiB none (T2) |

Deferred to later gates, as expected: history, session, thumbnail rendering, GUI, CLI and Python
tests.
