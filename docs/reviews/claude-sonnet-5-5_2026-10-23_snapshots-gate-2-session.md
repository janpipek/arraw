# Review: snapshots Gate 2, session lens

Reviewer: claude-sonnet-5-5, 2026-10-23. Subject: the uncommitted gate-1 follow-ups and Gate 2 working
tree (`include/EditSession.h`, `include/Sidecar.h`, `src/core/EditSession.cpp`, `src/core/Sidecar.cpp`,
`src/app/HistoryModel.*`, `src/app/ui/MainWindow.*`, `src/python/BindSession.cpp`,
`tests/test_SessionSnapshots.cpp`, `tests/test_HistoryModel.cpp`), read against ADR 056, the
architecture proposal and the Gate 2 entry of `docs/todo.md`. Lens: session only (history and base
semantics, failure atomicity, marks-only-changed Save, thread-safety assumptions, regressions of
existing session behaviour). Static review: nothing was built or run, no production code was
changed. Claims about behaviour come from reading code and tests. The one Qt claim I make (finding 7)
is from memory of the Qt 6 sources and is marked as such.

## Verdict

The history and base machinery is sound and follows the ADR closely. I traced Create (with and
without an open gesture, at position 0, mid-history), Restore, goTo/undo/redo, Delete across current,
saved, baseline and every history step, Discard, Save and the revision-conflict refresh, and found no
defect in those semantics. The base is carried by every step constructor (`appendStep`), by the
baseline, by `cancel`, `goTo`, `baselineIsCurrent` and `canUndo`/`canRedo`, so a base-only difference
is a step and a dirty state everywhere it should be. Create writes first and mutates after; the only
mutation that can follow the write is non-throwing. The new tests are well chosen.

Two things should be fixed before Gate 2 is ticked: a session built from a bare `Photo` silently
clears the saved base on every Save (finding 1), and the marks-only-changed Save protects nothing in
practice because `setMarks` is the only path that changes marks and it still writes everything
(finding 2). The rest is minor.

## Findings, most serious first

### 1. A session made from a `Photo` wipes the sidecar's saved base on Save (major)

`src/core/EditSession.cpp:177,182` and `include/EditSession.h:204`. `save()` now always calls
`writeSidecar(next, base_, marks)`, which sets `arraw:savedBase` to `base_` (`writeActive` with
`setBase = true`, `src/core/Sidecar.cpp:1814`, removing the property when the base is empty). The
`Photo` constructor, which stays public, builds a session with an empty collection and **no** base, so
`base_` is always empty there.

Failing scenario: the GUI (which now uses `openDocument`) saves with base S, so the sidecar holds
`savedBase = S`. A Python script does `EditSession(open_photo(path))`, edits, `save()`
(`src/python/BindSession.cpp:153` binds exactly this constructor, and Gate 5 has not landed). The
write sets `savedBase` to nothing. The snapshots survive; the photographer's working lineage does not,
and nothing reports it. Before this change `save()` used `writeSidecar(next)` and left the base alone,
so this is a regression for every non-GUI caller. The doc comment only says the constructor is for a
photograph "known to have no snapshots"; nothing enforces it.

Options:
- **A.** Remember that the base is unknown (a flag, or a `std::optional<std::optional<SnapshotId>>`)
  and have `save()` call a writer that does not set the base. Smallest change, keeps the constructor.
- **B.** Make the `Photo` constructor read the collection and saved base itself
  (`openDocument(photo.path())` for those two fields only). Costs a second sidecar read and can
  disagree with the photo handed in.
- **C.** Remove the `Photo` constructor and bind `OpenDocument` in Python now.

I would take A. Add a test: write a sidecar with a saved base, open it through `openPhoto`, edit and
Save, assert `readSidecar(...)->savedBase` is unchanged. No test covers the `Photo` constructor with
a populated sidecar today (the one test that uses it only checks the conflict).

### 2. Marks-only-changed Save delivers no protection; `setMarks` still writes everything (major)

`src/core/EditSession.cpp:172-174,191-197`. `changedMarks(saved_.marks(), next.marks())` is always
empty: `photo_` and `saved_` get the same marks in the constructor, `discardChanges`, `goTo` (which
only replaces state and negative) and `setMarks` (which sets both). So `save()` writes no marks, as the
implementer says, and the ADR 056 decision 3 sentence "a stale session does not put back a rating
another process set" is true only for Save. The one operation that does change marks, `setMarks`
(`writeSidecar(saved)` at line 195), writes the whole saved recipe **and** every mark from the
session's stale view.

Failing scenario: the window has unsaved edits, so `sidecarChangedOnDisk` keeps the session and shows
"your unsaved edits are kept". The CLI then sets rating 5 and saves a new recipe. The photographer
presses the red-label key; `setMarks` writes the session's `saved_` recipe (old) and its rating (0).
Rating 5 and the CLI's recipe are both reverted. The recipe revert predates this change, but the
rating half is exactly what the new `MarkChanges` machinery was written to prevent.

Second effect of the same invariant: a scripted `EditSession(photo.with(PhotoMarks{.rating = 3}))`
followed by `save()` used to put the rating in the file (patchActive wrote marks that differed from
the file); now it is dropped, because the session treats the photo's marks as already on disk. The
constructor doc does say so, but it is a behaviour change.

Fix (one line plus a test): `setMarks` calls
`writeSidecarMarkChanges(photo_.path(), changedMarks(saved_.marks(), marks))` and then updates
`saved_`/`photo_`. v2's `activeStatePresent` already makes a marks-only write safe for an untouched
RAW, which is the reason ADR 030 wrote the whole recipe. That also stops `setMarks` rewriting a stale
recipe. The existing test and ADR 030 sentence that assert the old behaviour need updating. If you
would rather keep ADR 030 for this gate, record in the ADR that the guarantee is for Save only and
keep this on the Gate 4 list, but do not leave the ADR sentence as it stands.

### 3. Save's retry mutates the session before the second write (minor)

`src/core/EditSession.cpp:178-183`. On `SnapshotBaseMissing` the first catch calls
`refreshSnapshots()`, which adopts the new collection and remaps `base_`, `savedBase_`, the baseline
and every history step. If the second `writeSidecar` then throws (lock timeout, I/O error, or the base
deleted again), `save()` leaves with the session already remapped. The doc says
"@throws std::runtime_error as writeSidecar does; nothing changes", and the Gate 2 brief asks for
failure atomicity. The remap is harmless to the data (it is what a refresh would do anyway), so this
is a documentation and test gap more than a bug. Either say "the collection and bases are refreshed
even if the retry fails", or compute the remapped base without adopting, write, then adopt on success.
Neither a retry that fails nor the double-delete case is tested.

`restoreSnapshot` has a smaller version of the same thing (`:286-303`): an open edit is committed
(line 292) before `photo_.with(snapshot.negative).with(snapshot.state)` (line 297) is built. If that
throws `std::invalid_argument` (a snapshot recipe the photograph rejects, for example after the raw
file was swapped), the commit has already happened and the doc's "nothing changes" is not true. Build
`next` before committing; it depends only on the snapshot, and `with()` keeps the photograph's
identity and marks.

### 4. The lens-bypass claim for Restore is untested (minor)

`tests/test_SessionSnapshots.cpp`, "Restore is an exact undoable step". The session has no
`LensDatabase`, so `withLensRecorded` returns its input and the test would pass if Restore were
rewritten to go through `setState(negative, state, EditOrigin::Restore, name)`, which is the
regression the proposal warns about. The implementer acknowledged this. `test::fixtureDatabase()`
exists (`tests/support/LensFixtures.h`); build a session with it, a snapshot whose negative has a
correction on and no lens recorded, and assert the restored `photo().negative()` has no lens recorded.
Also assert that a later edit that does not touch the lens settings still records none.

The code itself is correct: `restoreSnapshot` builds the photograph with `with()` and appends the step
directly; `goTo`, `undo`, `redo` never rematch; `save()` and `createSnapshot` only rematch under an
open edit.

### 5. Failure atomicity is tested for Create only (minor)

`tests/test_SessionSnapshots.cpp`. Only Create has a lock-timeout case. `renameSnapshot` and
`deleteSnapshot` have the same shape (write, then mutate), and I found them correct, but a regression
that mutates `replaceBase` before the write would not be caught. Add a lock-timeout case for each
that checks the collection, `baseSnapshot()`, `savedBaseSnapshot()` and every history base are
unchanged. The conflict test has no open edit, so "the edit stays open and its baseline is remapped"
on a Create conflict is also unproven; add one section with `begin()`/`update()` before the other
process deletes the base.

### 6. `sidecarChangedOnDisk` rebuilds the whole session for snapshot-only changes (minor, latent)

`src/app/ui/MainWindow.cpp:2000-2012`. The new condition `document.snapshots == session.snapshots()`
compares the entire collection, thumbnails included. Two consequences:
- Any external snapshot-only change (the CLI adds one, another window renames one) on a **clean**
  session discards the history and replaces the session, although `refreshSnapshots()` would do the
  job and keep the undo list.
- Gate 3 attaches thumbnails with `setSnapshotThumbnail`, which does not change the revision. The
  session's copy is then stale, the watcher fires, the equality fails, and every thumbnail landing on
  an idle photograph resets the session. ADR 056 decision 6 and the proposal say the watcher must not
  report the app's own thumbnail writes as external edits, and this comparison is the place that has
  to honour that.

Suggest: compare structure only (ignore `thumbnail`), and when only the collection differs call
`refreshSnapshots()` instead of rebuilding. Gate 4 is the natural place, but the equality was added
here, so note it in `docs/todo.md`.

### 7. The 5 s stale-lock time may let a slow holder be overtaken (minor, not verified)

`src/core/Sidecar.cpp:1596`, `include/Sidecar.h` (`sidecarStaleLockMilliseconds`). From memory of
Qt 6's `QLockFilePrivate::isApparentlyStale`, a lock is stale when its owner is not running **or** its
age exceeds the stale time, whatever the owner's state. The implementer's experiment on Linux found a
live owner's lock was not broken, so I may be remembering wrongly, and I could not run anything. If
the age rule applies, the scenario is a writer that holds the lock for more than 5 s (the in-lock
work includes reading the list, patching, serializing and checking up to 128 MiB, three times for a
structural edit, on a network folder) while a second writer takes the lock over and both do
read-modify-write; the later `QSaveFile` commit silently drops the earlier change. The old default
was 30 s. The test only asserts the constant. A real check would hold a `QLockFile` from a second
thread past the stale time and see whether `tryLock` in a writer succeeds. If it does, restore a
larger value or refresh the lock during long writes.

### 8. Thread-safety and API notes for Gate 4 (minor)

- Every new mutator does sidecar I/O synchronously: up to `sidecarLockTimeoutMilliseconds` (2 s) of
  waiting, plus a parse of the whole snapshot subtree. Called from a GUI action this blocks the GUI
  thread, which ADR 043 forbids for decoding and which `setMarks` already does for a much smaller file.
  The session is documented as not thread-safe (`setLensDatabase`), so the options are: accept the
  stall and say so in the Gate 4 notes; or run the sidecar write off-thread against a copy of the
  collection and apply the result on the GUI thread (this needs the write and the mutation split,
  which the code already almost has).
- `snapshots()` returns a reference that `createSnapshot`, `renameSnapshot`, `deleteSnapshot`,
  `refreshSnapshots` and a conflict all replace. A `const Snapshot*` from `find()` dangles after any of
  them. The tests happen to re-find each time. Document it on `snapshots()` before `SnapshotModel`
  keeps pointers.
- `refreshSnapshots()` is public and reads the file outside the lock. That is safe because `QSaveFile`
  replaces atomically, but say so; a future caller may assume it blocks writers.

### 9. Create leaves the session dirty with identical pixels (minor; implementer's question 4)

`include/EditSession.h:369`. This follows from the accepted design: after Create, `base_ != savedBase_`
so `hasUnsavedChanges()` is true, and a photographer who only captured a snapshot is asked to save on
leaving, and sees "your unsaved edits are kept" for their own sidecar write (the watcher fires on
Create). It is consistent with ADR 056's Consequences and with the proposal's worked example, so I
would not change the model, but decide the Gate 4 behaviour on purpose:
- **A.** Leave it. Cheap, consistent, a prompt on close. Make the quit prompt say "snapshot base
  changed" rather than "unsaved edits".
- **B.** Have Create also set the saved base when the session was clean before (no unsaved state or
  negative, base equal to saved base). That is an extra write inside the same lock, and it contradicts
  the ADR sentence "reopening shows those old settings and base", so it needs an ADR amendment.
- **C.** Keep it, but make the sidecar watcher ignore events it caused (compare against the document
  Create just returned).

### 10. Documentation nits (minor)

- `include/EditSession.h:407` (Create): "nothing that could be redone is dropped" is not true when an
  open gesture changed something; its commit is an ordinary step and drops the redo list. The tests
  (`Create settles a gesture`) rely on exactly that. Say "settling an open edit follows the ordinary
  commit rule".
- `include/EditSession.h:408`: "If the write fails, nothing changes, an open edit included" is true;
  add that a revision conflict also leaves the edit open.
- `HistoryModel::setHistory` (`src/app/HistoryModel.h:42`) defaults `savedBase` to `std::nullopt`, so a
  caller that forgets it gets the wrong saved marker without a compile error. `MainWindow` is the only
  caller; drop the default and pass it in the seven test calls, or keep it and accept.
- ADR 056 decision 3 and `docs/todo.md` item 2 were edited without re-wrapping to 100 columns.

## Checked and found correct

- **Create anchoring.** `createSnapshot` computes the settled photograph (lens recorded) before the
  write, writes, then commits the gesture and sets `history_[position_].baseSnapshot`. With a changed
  gesture the new step carries the new base and the preceding step keeps the old one; with a no-op
  gesture or no gesture the current step is anchored and no step is added; redo steps keep their own
  bases (the proposal's worked example, tested). At position 0 the Opened step is anchored, so Undo is
  unavailable when nothing precedes it. `discardChanges` rebuilds the Opened step from the saved base.
- **Equal-pixel base change.** `commitPhoto`, `baselineIsCurrent`, `canUndo`, `canRedo`, `cancel`,
  `goTo` and `restoreSnapshot` all compare or restore the base. The `setState(negative, state)`
  early-out does not compare the base, which is right: it is only reached for ordinary edits, where the
  base does not change.
- **Delete remapping.** `replaceBase` covers `base_`, `savedBase_`, the baseline and every step, using
  the parent from the held collection, which equals the file's at the matching revision. The file moves
  its own saved base the same way, so session and disk agree (tested). A conflict-time `adopt` uses the
  nearest surviving ancestor in the held list, which is also the file's reparenting rule.
- **Dirty and marker.** `hasUnsavedChanges`, `HistoryModel::SavedRole` and `discardChanges` agree on
  the three-way comparison; the HistoryModel tests cover both directions.
- **Constructor.** Member initialization order matches declaration order, and `document.photo` is
  copied before it is moved.
- **Existing behaviour.** The pre-existing session tests are not touched by the diff apart from the
  new field; the `HistoryStep` aggregate gains a defaulted last member, so every existing brace
  initialization still compiles and `operator==` now also compares the base, which is intended. I found
  no switch over `EditOrigin` outside `HistoryModel` and the Python enum that misses `Restore`.
- **Restore on a stale view.** Restoring a snapshot another process has deleted succeeds from the held
  collection (Restore does no I/O), and the dangling base then surfaces at Save as `SnapshotBaseMissing`,
  is remapped to the nearest survivor and the save goes through. The recipe the photographer restored is
  kept, the lineage is shortened. Acceptable; the status line in Gate 4 could mention it.

## Not verified

Nothing was built or run, including the Python bindings (the `RESTORE` enum value and the stub are the
only changes there). Finding 7 depends on Qt behaviour I could not test.
