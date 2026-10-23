# Review: snapshots Gate 2 and gate-1 follow-ups, tests lens (uncommitted working tree)

Reviewer: Claude Sonnet 5.5, 2026-10-23. Scope: the uncommitted changes implementing the gate-1
follow-ups and Gate 2 (session history) of
[ADR 056](../adr/056-snapshots-are-per-photo-looks-with-independent-history-and-sidecar-storage.md),
read against its Verification list, the worked example in
[`snapshots-architecture-proposal.md`](../ideas/snapshots-architecture-proposal.md) and the Gate 2
entry of [`todo.md`](../todo.md). Lens: do the tests prove what the ADR and the proposal say they
must. Files read: `tests/test_SessionSnapshots.cpp` (648 lines, new), the diffs to
`tests/test_Snapshots.cpp`, `tests/test_HistoryModel.cpp`, `tests/CMakeLists.txt`, and the
implementation in `include/EditSession.h`, `src/core/EditSession.cpp`, `src/core/Sidecar.cpp`,
`src/app/HistoryModel.cpp`, `src/app/ui/MainWindow.cpp` as far as needed to see what an assertion
can and cannot catch. I did not build or run anything, so every "would pass" below is by reading,
not by mutation.

**Counts:** major 3, medium 6, minor 6.

Summary: the session tests are good where they matter most. The A/B/C worked example is
reproduced step for step and its Undo/Redo path is checked at each hop. Create's "no step, redo
kept, earlier base reached by Undo", the exact-Restore step, "a base change is a step", Save's
stale-rating case and the Save retry after a deleted base are all asserted with values that
distinguish the right behaviour from the obvious wrong one. No existing test was weakened except
one gate-1 thumbnail scenario (T4). The weak spots are three claims whose tests cannot fail: the
Restore "bypasses lens rematching" test has no lens database, the Delete test does not exercise
the open-edit baseline remap it says it covers, and the stale-lock test compares a constant with
its own literal. Failure injection is thin: only Create is tried under a lock timeout, and the
new Rename, Delete, Save and refresh paths are never made to fail.

Existing-test edits: the 14 `setSnapshotThumbnail` call sites are ported mechanically and still
mean what they did, except the rename-in-between case (T4). The `HistoryModel` saved-marker test
is discriminating in both directions. `tests/test_EditSession.cpp` is untouched, so the
`Photo`-constructor wrapper is covered by the whole old suite, which is the right evidence for
"nothing changed".

---

## Major

### T1. "Restore bypasses lens rematching" is not tested, and the test says it is

`tests/test_SessionSnapshots.cpp:237-267`. The negative at `:240-242` has `lens.distortion = true`
with the comment "On, with no lens recorded: a rematch would record one." The session comes from
`Rig::open()` (`:44-46`), which is `EditSession(openDocument(photo))` with no lens database
(`lensDatabase_` is `nullptr`), over `linear-32x24-neutral.dng`, a fixture with no lens. With no
database `withLensRecorded` returns its input, and with no lens in the metadata nothing could be
matched anyway. A `restoreSnapshot` that did call `withLensRecorded` (or `recordLensMatch`)
passes this test unchanged. The proposal states the requirement outright ("It must bypass
automatic lens rematching") and the implementer lists it as a known gap, but the test comment
reads as if it covered it, which is worse than no comment.

Failing scenario: change `restoreSnapshot` to `Photo next = withLensRecorded(photo_.negative().lens,
photo_.with(snapshot.negative).with(snapshot.state));`. On a Canon photo with a database,
Restore of a snapshot with the correction on and no recorded lens would now record one and the
step would no longer equal the snapshot. Every test here still passes.

Fix: the machinery exists. `tests/test_LensSettings.cpp:394-470` uses `ARRAW_REQUIRE_LENSFUN_OR_SKIP()`,
`lensFixture`, `test::fixtureDatabase()` and `EditSession(photo, &database)`. Write a snapshot with
`addSnapshot` (lens switches on, `lens.lens` empty, which `Create` could never produce on a session
that has a database, because it matches), refresh the session, restore it, and require
`step.negative == snapshot.negative` and `!step.negative.lens.lens`. Add the mirror test for Create
settling a gesture with a database: the stored snapshot carries the recorded lens, as the comment
at `EditSession.cpp:createSnapshot` ("matched before anything is written") says. That path has no
test either. Correct or delete the comment at `:242`.

### T2. The Delete test does not exercise the open-edit baseline remap

`tests/test_SessionSnapshots.cpp:518-563`, specifically `:527-542`. The test opens an edit
(`begin`, `update(3)`), deletes C, commits, and requires the new step's base to be B. But `commit`
appends a step whenever the *state* differs, and the state differs (3 versus 2), so
`before.base != base_` is never what decides. The step is stamped with `base_`, which `replaceBase`
fixed whether or not `baseline_->base` was remapped. Remove `swap(baseline_->base)` from
`replaceBase` and `baseline_->base = survivor(...)` from `adopt` and this test, and the conflict
test, still pass.

What the remap is for, and nothing checks:

- `cancel()` restores `base_ = baseline_->base`. Without the remap, `begin(); delete C; cancel()`
  leaves the session based on a snapshot that no longer exists (the base then fails at the next
  Save with `SnapshotBaseMissing`, or worse, is retried and remapped silently).
- `baselineIsCurrent()` compares `baseline_->base == base_`. Without the remap, `begin(); delete
  C; commit()` appends a spurious Edit step with identical pixels.

Fix: two short cases. (a) `begin(); update(x); deleteSnapshot(c); cancel();` expects
`baseSnapshot() == b` and the original state. (b) `begin(); deleteSnapshot(c); commit();` expects
no new step. The same pair for the conflict path (`adopt`) would use the fault-injection helper
suggested in M3.

### T3. The stale-lock test cannot fail

`tests/test_Snapshots.cpp:1062-1068`. `static_assert(sidecarStaleLockMilliseconds == 5000)` and
`CHECK(detail::sidecarStaleLock == 5000)` compare two constants with a literal. Delete
`file_.setStaleLockTime(detail::sidecarStaleLock)` (`Sidecar.cpp:1596`) and both assertions hold;
set it to a different value in `SidecarLock` and they hold. The follow-up was "Qt's default is
implicit, set it explicitly", and nothing observes that it is set. The knob
`detail::sidecarStaleLock` is documented "lowered only by tests" (`SidecarLimits.h`) and no test
lowers it, so the infrastructure for a real test was added and left unused. The ADR text now also
promises "a lock older than 5000 ms is stale and taken over", which no test shows for a live
owner.

The implementer's note says a lock whose owner is alive was not broken by age in an experiment on
Linux. That may be right (Qt's age check sits behind the process-liveness check and has
platform-specific wrinkles), but then the ADR sentence is unproven rather than tested, and
the test title "The stale lock time is five seconds" over-claims.

Options:

1. Lower `detail::sidecarStaleLock` to e.g. 200 ms, write a lock file whose pid is `getpid()` and
   whose application name is the test's (copy the three-line content from a `QLockFile` held and
   released as `:1042-1049` already does, but keep the live pid), back-date its mtime, and expect a
   writer to succeed within the 2 s wait. If Qt will not break it, that is an answer, and the ADR
   and `Sidecar.h` wording should say "a lock whose owner is gone", which is what the existing
   test at `:1036` shows.
2. Keep a constant check but make it fail on removal: expose `detail::sidecarStaleLock` as the
   thing the lock reads, and test via a seam (a function that builds the configured `QLockFile`,
   whose `staleLockTime()` Qt 6 does expose; Qt 6.x has `QLockFile::staleLockTime()`, which
   contradicts the "no getter" remark, so check the version in use first).
3. Drop the test and the "5000 ms" sentence from the ADR until measured.

---

## Medium

### T4. The rename-in-between thumbnail scenario no longer tests a rename

`tests/test_Snapshots.cpp:594-601`. Before, `currentKey(node)` was evaluated under the lock, so a
rename on disk changed the key and dropped the late thumbnail. Now `expectedKey` is a caller
string compared with the thumbnail's own `renderKey`. The scenario still performs
`renameSnapshot(...)` and then calls with thumbnail key "key-First" and expected key
"key-Renamed", so it passes because the two strings differ, not because anything about the node was
read. The comment ("A rename in between changes the key, so the late thumbnail made for the old
name is dropped") describes a mechanism that is gone; delete the `renameSnapshot` line and the
assertion still passes. This is the implementer's open question 1 seen from the test side: the
guard is against the caller's staleness only, and the test should say that.

The other call sites pass the thumbnail's own key as the expected key (`:1018` lock test,
`:1141-1143` concurrent writers, `:1252`, `:1684-1687`), so on the happy and race paths the
comparison is a tautology. That is acceptable for an immutable recipe, but then no test shows a
mismatch under concurrency or that a node deleted and its ID never reused (the real protection
now) holds: `:611-616` shows the deleted case only sequentially.

Fix: rewrite the comment to what it checks ("the caller's expected key differs from the
thumbnail's: dropped"); add one test where the sidecar's node and an unchanged expected key
disagree only if a design with a stored recipe hash is chosen (open question 1, alternative).
If the plain key stays, say in the ADR that thumbnail validity rests on immutable recipes and
unique IDs, and test that `nextId` is never reused after delete (gate 1 may already; I did not
re-check).

### T5. A `Photo`-built session saves the base away, and no test says what happens

`include/EditSession.h` (`explicit EditSession(Photo, ...)`) and `src/python/BindSession.cpp:153`.
The wrapper starts with an empty collection and `savedBase == nullopt`; `save()` then calls
`writeSidecar(next, base_, marks)` with `base_ == nullopt`, which writes `savedBase` as absent
(`Sidecar.cpp` `writeActive`, `setProperty(... std::nullopt)`). A Python user who opens a photo
whose sidecar has a saved base A, edits and calls `session.save()` silently removes the saved
base from disk. `tests/test_SessionSnapshots.cpp:619-636` pins the Create conflict for this
construction but not the Save.

Failing scenario: sidecar with snapshot A and `savedBase = A`; `EditSession s(openPhoto(p));
s.setState(x); s.save();` then `openDocument(p).savedBase` is empty.

Fix: either a test that pins "the legacy constructor drops the saved base" (and an ADR/doc note),
or, better, the wrapper reads the document (`openDocument`) so the data is not lost, with the
test above asserting the base survives. The Python bindings need the same test in
`tests/python/test_session.py` (not built or run here).

### T6. Revision-conflict and refresh paths are tested one level deep, and refresh failure is untested

`tests/test_SessionSnapshots.cpp:581-648`. The survivor walk (`EditSession.cpp:survivor`) is what
the Delete-by-another-process and conflict cases depend on, and the tests only delete a node whose
parent survives (`b -> a`). Not covered:

- two deleted ancestors in a row (C -> B -> A, B and C gone, expect A), and a chain that ends at
  no survivor (expect `nullopt`);
- the saved base and a history step other than the last remapping on refresh, as opposed to the
  working base (`:612` checks only `baseSnapshot()` and the last step; `:644-647` checks only a
  rename);
- `refreshSnapshots` when the sidecar has gone (`readSidecar` returns `nullopt`, the code adopts
  an empty collection, so every base becomes `nullopt` while the saved recipe stays): a plausible
  "user deleted the sidecar in the file manager" case, and the unreadable-file throw ("nothing
  changes") has no test either;
- `refreshAfterConflict` swallowing a read failure so the *conflict* is what is thrown (`noexcept`
  catch-all at `EditSession.cpp:refreshAfterConflict`).

### T7. Failure injection covers Create only

Only `:151-181` makes a write fail. The ADR's "Timeout or I/O failure leaves the prior file
unchanged and reports a clear error" and the proposal's "On write failure, neither the node nor
the current position's base changes" are tested for Create under a lock timeout and nothing else.
Untested at session level:

- Rename and Delete under a lock timeout or a non-XML sidecar (the pattern at
  `tests/test_EditSession.cpp:352-370`): `snapshots_`, the bases and the history must be as
  before. In `deleteSnapshot` the order (disk first, then `replaceBase`) makes this true today; a
  swap would be silent.
- Create when the list is full (32), when the name is too long, and with the sidecar not XML.
- Save failing with a base present: `savedBase_` stays old, the open edit stays open
  (`test_EditSession.cpp:352` runs without a base).
- Save whose retry fails again: `SnapshotBaseMissing` is documented, but the first attempt's
  `refreshSnapshots()` has already remapped `base_` and the history, so "nothing changes" is not
  literally true and no test says which it is. No test makes the base vanish twice (it needs a
  hook between the attempts; an `ARRAW`-internal fault seam, or a thread, would do).
- The `ShortLockTimeout` plus `QLockFile` pattern at `:161-167` is cheap; reuse it per operation.

### T8. Marks: the new session path is unreachable and the interesting edges are untested

`EditSession.cpp:save()` computes `changedMarks(saved_.marks(), next.marks())`. `setMarks` updates
`saved_` and `photo_` together, so the result is always empty in a session; the code path where a
session writes a mark is dead and the test at `:425-438` can only show the empty case (it does
that well: rating 5 and Red survive, and the old `writeSidecar(next)` would have written 0).
`:440-467` checks `changedMarks` for rating 1 to 2 and label clearing, but not:

- a rating cleared to 0 (`changedMarks({.rating = 3}, {})` must name rating 0; a truthiness test
  would drop it, and `writeSidecarMarkChanges` takes `MarkChanges{.rating = 0}`);
- both marks at once; `-1` (rejected) as a rating;
- `writeSidecarMarkChanges(photo, {})` writing nothing and not creating a sidecar
  (`Sidecar.cpp:writeSidecarMarkChanges` returns early; the file is untouched);
- an invalid rating throwing *before* any write (`:463-466` checks the exception, not that the
  file is unchanged; for the three-argument `writeSidecar` the check is before `writeActive`, but
  nothing asserts it).

The implementer's open question 2 (stale `setMarks` still overwrites a rating) has no test that
pins current behaviour, so a later switch to `writeSidecarMarkChanges` would change behaviour with
no test to flag it, and ADR 030's claim is asserted only by an old test the report mentions.

### T9. Save and the collection: no session-level test that Save leaves other processes' snapshots alone

The proposal's "Ordinary Save: ... preserve the latest snapshot collection on disk" is covered at
the sidecar level in gate 1, and the session tests save only after the session's own Create.
Missing: another process adds a snapshot after the session opened; `session.save()` succeeds (no
revision check on Save) and the new node is still on disk afterwards; and the session's
`snapshots()` is then stale until a conflict (the code does not refresh on Save). That staleness is
a design choice worth pinning in a test so Gate 4's snapshot model, which will display
`snapshots()`, is written against it. Likewise `setMarks` after a Save with base A must leave
`disk.savedBase == A` (the one-argument `writeSidecar` does not touch the base).

---

## Minor

### T10. "Saving writes the base" proves the dirty flag by the wrong cause

`tests/test_SessionSnapshots.cpp:381-400`. After Save, `session.undo()` goes from exposure 1 to the
default and `hasUnsavedChanges()` is true because the state differs. The comment says "undoing
returns to the saved-base-of-the-past and is dirty again"; the base never matters. It does not
fail if `hasUnsavedChanges()` ignored `base_`. The test at `:369-379` and the Restore test at
`:269-290` are what cover the base term, and they do so properly. Drop the claim or undo to a step
with the same pixels and a different base.

### T11. `goTo` visits no `nullopt` base and the Restore undo check is not discriminating

`:352-367` visits steps with bases `A, A, A, B`; every move goes between two non-empty bases, and
`goTo(1)` after `goTo(3)` is the only hop that would fail if `goTo` forgot `base_`. Add a step
whose base is empty (the first, before any Create). `:263` claims "the step before it was anchored
to A by Create"; it was not (it was an edit made while A was the base), and since A is the base
before and after the Restore, undoing across the Restore cannot show that the base is carried.
The worked example (`:314-350`) does show it, so this is only about the test's own claim.

### T12. The worked example is faithful; two details of the proposal it does not reach

`:314-350` follows "with A as base: edit, restore B, edit, create C" exactly, and checks C's
parent, the base of the final position, Undo to the Restore-B position (origin, base, pixels), Undo
across the Restore to the A-based work, and Redo to the captured position with the new base. Not
checked: the intermediate Redo position (one `redo()` should land on Restore-B with base B, not
yet C); and the proposal's last paragraph, "Capturing C does not erase any redo positions", is not
part of the example (the Create-keeps-redo case at `:183-216` covers it with a single snapshot
and no Restore). One example that Creates while redo entries span a Restore would close the gap.

### T13. Gate-1 error-message tests

`tests/test_Snapshots.cpp:1380-1397`. Good: the 32-entry limit names "12 unreadable entries", and the
rename of a missing ID says "no usable snapshot 25" plus the note. The plain-refusal case at
`:1388-1397` calls `deleteSnapshot` twice and does the `!ContainsSubstring("unreadable")` check
inside a `catch`, so it passes silently if nothing throws; the first call already proves the throw,
but a single `CHECK_THROWS_WITH(..., ContainsSubstring("no usable snapshot 7") &&
!ContainsSubstring("unreadable"))` is tighter. `unreadableNote` returns text when `read.damaged`
is set even with zero unreadable nodes, which would print "holds 0 unreadable entries"; no test
reaches that arm. Delete is not tried with unreadable entries, and no test confirms what a
thumbnail patch for such an ID returns (it returns false, with no message).

### T14. Not covered by any test: GUI and Python

`MainWindow::sidecarChangedOnDisk` now also compares the saved base and the collection; no widget
test covers the reload decision (neither "snapshot added elsewhere reloads a clean session" nor
"own Create does not reload"). The latter holds only because `session.snapshots()` equals what
`openDocument` returns, which `:126`, `:552` and `:578` do assert, so the key invariant is tested
indirectly. The Python `RESTORE` value and `_arraw.pyi` were edited, not built, and
`tests/python/test_session.py` has no case for it. `HistoryStep.baseSnapshot` is not bound, so Python
cannot observe a Restore's base.

### T15. Housekeeping

`tests/CMakeLists.txt` puts `test_SessionSnapshots.cpp` between `test_Sidecar.cpp` and
`test_Snapshots.cpp`; the list is otherwise alphabetical (`test_Session*` sorts before
`test_Shot`). The tests tag `[session][snapshots]`; confirm `just` test filters do not exclude
them. `Rig::open()` is `[[nodiscard]]` and used well; the bare `Rig` copies a DNG per case, which is
fine at 22 cases.

---

## What is well covered

- Create: writes at once, parent is the working base, no history step, redo kept with the later
  steps' own bases, Undo reaches the earlier base, gesture settled first, failure under a lock
  timeout leaves an open gesture open. Dirty after Create is pinned (open question 4).
- Restore: exact state and negative, `EditOrigin::Restore`, name in detail, base-only change is a
  step, no-op when both match, drops redo, commits an open edit first.
- Delete: reparent, saved base remapped on disk and in the session, every history step remapped,
  Undo does not bring the node back, nothing referenced after the last delete.
- Save: writes the base, retries once after a deleted base (history and saved base remapped),
  keeps a rating another process set, Discard keeps durable snapshots.
- `HistoryModel`: the Restore wording and the saved marker compare the base in both directions.
- Revision conflicts: Create, Rename and Delete each refresh and rethrow; Create does not retry
  (entry count 2, not 3).
