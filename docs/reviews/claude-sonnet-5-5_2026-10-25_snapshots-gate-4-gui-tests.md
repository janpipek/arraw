# Review: snapshots Gate 4 (GUI), gui-tests lens (uncommitted working tree)

Reviewer: Claude Sonnet 5.5, 2026-10-25. Scope: the uncommitted STEP B changes (GUI) read against
[`docs/ideas/snapshots-gui-plan.md`](../ideas/snapshots-gui-plan.md), ADR 056 (decisions 11 to 17
and the Verification list) and the Gate 4 entry of [`todo.md`](../todo.md) ("Verify preview versus
restore, edit disabling, and refresh after external changes"). Lens: `SnapshotModel` correctness
(tree, reparent on delete, roles, placeholders); preview and Restore never mutating the session;
editing disabled during a preview; dirty and Save-prompt behaviour; crop and gesture not
disturbed; refresh after an external change; UI-thread blocking; and whether the widget tests
prove the ADR list or are vacuous or timing-flaky.

Files read: `src/app/SnapshotModel.{h,cpp}`, the full diff of `src/app/ui/MainWindow.{h,cpp}`,
the diffs of `FilmStrip.{h,cpp}` and `HistoryModel.cpp`, `tests/test_SnapshotModel.cpp`,
`tests/test_MainWindowSnapshots.cpp`, `docs/keybindings.md`. I did not build or run anything, so
every "would still pass" below is by reading, not by mutation. STEP A (core, writer) is outside
this lens.

**Counts:** major 4, minor 12.

Summary: `SnapshotModel` is sound. Its tree, index and parent arithmetic are consistent, the
reset-versus-`dataChanged` split matches the plan, and a delete's reparenting recomputes the
edges. The preview design (a separate shown photograph, the session untouched) is implemented
cleanly, and the editing gate is complete for the actions I could trace: `updateEditingActions`,
`setMaskMode`, `setPicking`, `copySettings`, `pasteSettings`, `editGeometry`, `pickNeutralAt`,
`chooseNegative`, the lens paths, the history list, undo and redo, and Save all test `editable()`
or `previewing()`. The two risks are one product-level hazard and one platform hazard. Export
stays on and silently exports a different look from the one on screen. `QTreeView::activated`
restores on a single click on desktops configured for single-click activation, which breaks "a
preview never mutates". The tests are good at the happy path of Create, Rename, Delete, Save
prompt, preview and the two own-write cases. They do not cover the "refresh after external
changes" half of the todo line beyond an added snapshot, a preview whose negative needs another
decode, or any failure path of the window's result slot.

---

## Major

### G1. `activated` restores on single-click platforms, so a click both previews and restores

`src/app/ui/MainWindow.cpp:1380-1384`; `tests/test_MainWindowSnapshots.cpp:436-464`.

The tree connects `QTreeView::activated` to `restoreSnapshot`. Qt documents that "how to activate
items depends on the platform, e.g., by single- or double-clicking". On a desktop configured for
single-click activation (KDE Plasma's default, through the platform theme's
`ItemViewActivateItemOnSingleClick`; Linux is the primary target) a single click first changes the
selection, which starts a preview, and then emits `activated`, which restores. The user wanted to
look and gets an undoable Restore step and a dirty session. Restore also does not ask, by the
plan's own decision, so nothing stops it.

The test cannot see this. `Window::select` calls `setCurrentIndex` on the selection model, and the
Enter section calls `QTest::keyClick(Key_Return)` on the tree. No test clicks a row with
`QTest::mouseClick` on the viewport, and none double-clicks, although the plan's test list says
"a click on a row" and the keybindings doc says double-click restores.

Fix options: (1) connect `doubleClicked` for the mouse and handle Enter/Return with a
`QShortcut` or a `keyPressEvent` filter on the tree, leaving `activated` unused; (2) keep
`activated` but ignore it when `QApplication::style()->styleHint(SH_ItemView_ActivateItemOnSingleClick)`
is true; (3) put Restore behind the banner and menu only. Option 1 is the smallest. Add one test
that clicks a row's viewport position and asserts the history length is unchanged, and one that
double-clicks and asserts a step was added.

### G2. Export stays enabled during a preview and exports a look other than the one shown

`src/app/ui/MainWindow.cpp:3833-3834` (enable), `2776-2800` (the job takes `photo.state()` of the
session); `tests/test_MainWindowSnapshots.cpp:393` (asserts the action is enabled).

The window shows snapshot S with a banner "Previewing S". Export, with a dialog whose frame size is
worked out from the working look, writes the working look. Nothing says so, and the export is a
file on disk. Every other action that stores the working look (Save) is disabled for exactly this
reason; deviation 3 of the report concedes the plan's wording ("saved settings") does not match
the code ("current state"). The test pins the questionable behaviour as intended, so a fix will
need to change it.

Options: (a) disable Export and the presets menu while previewing, consistent with Save and
Copy; it is one line in `updateEditingActions` and the test flips; (b) export the previewed
photograph, which is the GUI side of Gate 5's `export --snapshot`, passing `shownPhoto()` to the
job and using `decodeOptionsFor(shown)`; (c) keep it and add the previewed name to the export
dialog. I would take (a) now and (b) with Gate 5. This answers the report's first open question.

### G3. "Refresh after external changes" is tested only for an added snapshot

`tests/test_MainWindowSnapshots.cpp:551-582, 520-549`; `src/app/ui/MainWindow.cpp:2654-2730,
1688-1702`.

The todo line and the ADR list name "refresh after external changes". The two tests cover an
own write while cropping and one external `addSnapshot` during a mask gesture. Missing, and each
is a distinct branch of `sidecarChangedOnDisk` and `refreshSnapshotUi`:

1. Another program deletes the snapshot being previewed. `refreshSnapshotUi` computes `closed`,
   calls `closePreview(false)` and `refreshPanel`. Only the own-Delete path reaches the same
   code, so the external route (`adoptDiskSnapshots`) is unproven, including that the develop
   dock and history list come back.
2. Another program renames, deletes the working base, or deletes the saved base. The base marker
   and `Saved base` tooltip must follow through `adoptSnapshots`; nothing checks it from the
   window.
3. The active settings change on disk while a preview is open and the session is clean. The code
   rebuilds the session (`open_.emplace`), so the preview must end, the model clear and refill,
   and the dock re-enable. This is the one path where `previewed` is dropped by replacing the
   whole `OpenPhoto`, and `closePreviewShortcut_`, the banner, the selection and `historyView_`
   rely on later calls to settle. Not tested.
4. The same change with unsaved edits and a preview: the "your unsaved edits are kept" branch.
5. A rebuild while a structural write is in flight. The new `OpenPhoto` has no
   `structuralTicket`, so the actions re-enable and a second structural write can start before
   the first lands (it then conflicts; recoverable, but untested).

Add a parametrised case for 1, 2 and 3 using `seed`/`deleteSnapshot`/`writeSidecar` plus
`emit sidecarChangedExternally`, asserting the preview state, the model and the enablement of the
develop dock and history list afterwards.

### G4. No test of a preview whose negative needs another decode, or of a failed write at the window

`tests/test_MainWindowSnapshots.cpp` (absent); `src/app/ui/MainWindow.cpp:3768-3790, 3596-3623,
2012-2030, 1962-2005`.

The plan treats the 37 `session.photo()` uses as the riskiest part of the change and says a
preview with different decode options re-decodes. Every seeded snapshot differs from the working
look in exposure only, so `syncDecode`, `decodeLanded`, `failedOptions_` and the "Cannot decode
... still shown as decoded with" message have never run with `shownPhoto()` set to a snapshot.
The same holds for `lensView()` (keyed on the shown photograph's lens) and for the
`thumbnailEdge = 0` and `followWithThumbnail` suppression: no test shows the film-strip
thumbnail is not overwritten by a preview, which is the invariant behind "the strip shows the
working look".

The window's result slot also has no failure test beyond the lock timeout: the Conflict branch
("changed by another program"), a Refused Create (list full, over-long name), a thumbnail write
failure (`thumbnailWritesFailed`), and the owner/serial drop of a result for a photograph left
meanwhile (deviation 11, added without a test). A conflict is easy to stage: seed externally
without delivering the watcher event, then Create from the stale session, and expect the status
message, the refreshed row and an unchanged base.

---

## Minor

### G5. Starting a second preview that fails leaves the window in a half-closed preview

`src/app/ui/MainWindow.cpp:1654-1667`.

The catch block resets `previewPhoto` and `previewed` but does not call `updateEditingActions()`
or `refreshPanel()` (the success path does both, `closePreview` does both). If a preview of S1 is
open and `Photo::with` throws for S2, `previewing()` becomes false while the view still shows S1,
the panel still shows S1's values, the develop dock and history list stay disabled, the Esc
shortcut is off, and the banner is hidden. Clicking another row recovers it. Reach it by a
snapshot whose recipe passes the sidecar parser but is refused by `Photo::with` for this photo.
Fix: in the catch, call `closePreview()` first if a preview was open (so the old one ends
cleanly), then show the message.

### G6. After one thumbnail write fails, no thumbnail is shown for the rest of the session

`src/app/ui/MainWindow.cpp:2014-2016` (set at `1970`);
report deviation 2.

`requestSnapshotThumbnails` returns at once when `thumbnailWritesFailed` is set, so a lock
timeout on one batch means a snapshot created later never gets even an in-memory picture: the
worker is not asked, and `setRendered` is never called. Only the write needs to stop. Keep
requesting and rendering, skip `submitSnapshotWrite` (the check is already in
`snapshotThumbnailLanded`), and clear the flag on a successful structural write.

### G7. `Ctrl+N` test ends in an assertion that cannot fail the way its comment says

`tests/test_MainWindowSnapshots.cpp:345-347`.

"Nothing else was saved: the sidecar holds no develop state" is checked as
`CHECK_FALSE(document.photo.state() == w.disk().photo.with(DevelopState{}).state())`. Both sides
come from the sidecar, and the right side is the default state. The line passes when exposure
0.75 was saved (state differs from default) and, evidently, when it was not (the opened RAW's
state differs from a bare `DevelopState{}`). It would not notice Create writing the active state,
which is the ADR 056 decision 3 invariant. Replace with
`CHECK(w.disk().photo.state().settings.tone.exposure == 0.0F)` and
`CHECK(w.disk().savedBase == std::nullopt)`. The test title says "off the UI thread" but only the
lock test shows that; rename or drop the clause.

### G8. The preview render comparison may be satisfied by the working look's final render

`tests/test_MainWindowSnapshots.cpp:381, 399, 411-413`; `Window` constructor lines 144-146.

`before` is `wholeFrameImage()` captured as soon as it is non-null and the listing ended. That
accessor returns "the render if it shows all of it, else the reduced one beneath", so `before` can
be a coarser stand-in with the final render still to come. Then `waitForRenderOtherThan(before)`
returns on the working look's own final render, the preview assertion is vacuous, and the final
`== before` could fail intermittently. The three repeat runs say it holds on this machine. Make
the assertion semantic: exposure +2.0 must be brighter, so compare mean luminance of
`wholeFrameImage()` before and during the preview (and after Esc), after waiting for the render
indicator to be idle.

### G9. The own-write test cannot tell the writer's stamp from the second line of defence

`tests/test_MainWindowSnapshots.cpp:520-549`.

It emits `sidecarChangedExternally` itself. Since the 3.6 reorder, `sidecarChangedOnDisk` returns
early when the disk equals the session whatever stamp was noted, so deleting
`filmStrip_->noteOwnWrite(...)` from `snapshotWriteLanded` (`MainWindow.cpp:1911`) leaves the test
green. The plan allowed this, and it does prove "no mode is left", which is the Gate 3 deferral. It
does not prove the first line of defence. Also, `waitForThumbnails` looks at the disk, not the
session, so whether the last thumbnail has been adopted when the event is emitted is up to queue
timing; the test passes either way but then covers `adoptDiskSnapshots` or not at random. Add the
real route: after the thumbnails land, wait for the actual `QFileSystemWatcher` event (or
`FolderWatcher::simulateDirectoryChanged` with the real sidecar) and assert the status line stays
empty and `noteOwnWrite` recorded the writer's stamp.

### G10. The lock-held test has a 200 ms margin against the scheduler

`tests/test_MainWindowSnapshots.cpp:633-668` (timing at 654-658).

The lock timeout is 300 ms and the tick is 100 ms. `CHECK_FALSE(createSnapshotAction.isEnabled())`
holds only while the write is still waiting, so a loaded machine that delays the test thread by
200 ms finds the failure already delivered (and `updateSnapshotPanel()` runs before the warning
box), and the check fails. Raise the timeout to 1500 ms and keep the tick at 100 ms; the case is
already `[slow]`. The same test asserts `createSnapshotAction.isEnabled()` after the warning,
which is true the moment the result lands (`snapshotWriteLanded` clears the ticket first), so it
proves nothing about `lock.unlock()`.

### G11. Assertions inside timer callbacks abort the test binary

`tests/test_MainWindowSnapshots.cpp:94-118`.

`typing` and `clicking` use `REQUIRE`/`FAIL` inside a `QTimer` slot that runs from the dialog's
nested event loop. A Catch exception thrown there passes through Qt's event dispatch, which does
not support exceptions from slots; the usual result is `std::terminate`, so the whole
`arraw-widget-tests` run dies with no case name instead of one red case. It would trigger the
day an unexpected modal appears (for instance a "Cannot Rename Snapshot" box meeting `typing`).
Record the problem in the `Answer` (`failure` string, close the modal with `reject()`), and check
it on the test thread after `exec` returns.

### G12. Several behaviours the plan lists have no widget test

`tests/test_MainWindowSnapshots.cpp` (absent); plan section 5, acceptance 3.

- The panel disabled in the crop mode and in the mask mode (the reason for `quiet`), and Esc and
  Delete still going to the mode: only the dock's crop-mode disabling is covered by older tests.
- Starting a preview disarms the colour picker so the two Esc handlers never both exist
  (`setPicking(false)`, deviation 7).
- `Del` on the tree (only F2 is driven by key; Delete uses `trigger()`).
- Restore from the menu, and by double-click.
- Create with a full list (the pre-check message that names both counts), and an over-long name
  (the re-prompt with the text kept).
- A preview does not change the undo and redo actions' state after Restore (`updateHistoryActions`
  is only checked for `undoAction` while previewing).
- Closing the window with a structural write pending, which the plan leaves to the hand check
  (acceptance 5) that was not done.

### G13. `SnapshotModel::setSnapshots` copies every thumbnail on every call

`src/app/SnapshotModel.cpp:120-124`.

`old` is a `std::vector<Snapshot>` copy of the held nodes, thumbnails (up to 64 KiB each, 32
snapshots) included, made only to compare structure with `std::ranges::equal`. It runs on the UI
thread on every forced refresh (each thumbnail landing, each structural result, each external
event) and then `node.snapshot != next` compares the JPEG bytes again. About 2 MiB per call is
small, but 32 thumbnails land one by one at open. Compare against `nodes_[i].snapshot` directly
(no copy), and compare thumbnails by `renderKey` where the key is the identity.

### G14. Model tests skip the fallback and edge branches

`tests/test_SnapshotModel.cpp` (absent); `src/app/SnapshotModel.cpp:85-113, 232-300`.

Untested: an undecodable stored JPEG falls back to the placeholder and keeps a `setRendered`
picture (`:104-110`; this is also the branch that would print `qt.gui.imageio.jpeg: Not a JPEG
file`, so the report's open question about that noise is worth one test that expects it); a
`setRendered` picture surviving a reset; `data()` on an invalid or foreign index;
`rowCount(parent)` and `index()` for a non-zero column; the roles carried by `dataChanged`; and
the tooltip's created-time line (the "Every role" test checks the change line and "Saved base"
only). `CHECK(model.data(warm, Qt::DecorationRole).canConvert<QImage>())` is satisfied by the
placeholder, so it checks nothing about a stored picture. The remaining model tests are sound and
discriminating (the reparent case recomputes the edge by `wordEdit`, which I checked against the
reset path in `link()`).

### G15. Rebuilding the session on an external change drops the in-flight ticket

`src/app/ui/MainWindow.cpp:2711-2722`.

`open_.emplace(OpenPhoto{...})` default-constructs `structuralTicket`, so while a Create/Rename
from the old session is still at the writer, the new session's actions are enabled and a second
structural write can be submitted on a revision the first is about to change. It ends in a
Conflict and a refresh, so it is not corruption. Carry the ticket across (it is a per-file
property, not per-session), or leave the actions disabled until the stale result arrives and is
dropped.

### G16. `refreshSnapshotModel(false)` keys on revision, count and the two bases only

`src/app/ui/MainWindow.cpp:1583-1600`.

Thumbnail and name changes inside an unchanged revision need `force`, and the call sites that
matter pass it (`refreshSnapshotUi`, the thumbnail branch). Any later path that changes the
session's collection without raising the revision or the count (for instance a Save that adopts a
collection another process rewrote with a thumbnail, through `refreshAfterConflict`) would leave
the tree stale until the next change. Not a bug today; a one-line guard would be to add the
session's `unreadableSnapshots()` and a cheap hash of the thumbnail keys to `ShownSnapshots`.

### G17. Observations without a finding

- Selection equals preview (the report's coupling question): each Up/Down in the tree starts a
  preview, which finishes a pending edit, rebuilds the panel and asks for a render. The render
  requests coalesce, so there is no UI-thread cost beyond `Photo::with`, but Rename and Delete
  cannot be used on a snapshot without previewing it. I would keep the coupling, and fix G1 so
  that it is safe. A separate gesture would cost a second selection concept.
- The `Esc` shortcut (`MainWindow.cpp:1394`) is window-level and enabled only while previewing;
  the crop, mask and picker Esc shortcuts cannot be on at the same time (the tree is disabled in
  both modes, the picker is disarmed on start), so there is no ambiguity. The one gap is a
  preview that has ended by replacing `OpenPhoto`; `updateEditingActions()` re-disables it.
- No synchronous disk I/O was added to the UI thread by the actions: prepare and finish are pure,
  `askSnapshotName` and the confirmation boxes run nested event loops, and the writer delivers by
  queued call. `sidecarChangedOnDisk` still reads the sidecar (with up to 32 thumbnails) on the
  UI thread for every watcher event that is not noted; that is as before, but the ADR's timing
  measurement (not taken) is the place to find out whether it needs to move.
- The destructor of `MainWindow` joins the writer after running its queued structural write, so
  closing with the sidecar locked by another process can block the window's teardown for one lock
  timeout. Acceptable; worth a line in the hand check.
- Answers to the report's open questions: Export, see G2. Copy Settings during a preview: keep it
  disabled until Gate 5; copying the on-screen look needs the same `shownPhoto()` plumbing as
  Export option (b). The STEP A error text: keep it neutral, since the CLI and Python see it too.

---

**Verification:** read only. Reasoned traces: selection to `startPreview`; `closePreview`;
`restoreSnapshot` through `guarded`; `snapshotWriteLanded` for each failure kind;
`sidecarChangedOnDisk` for each branch of 3.6; `refreshSnapshotUi` with a vanished preview.
