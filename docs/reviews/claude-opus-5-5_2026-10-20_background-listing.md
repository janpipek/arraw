# Review: background folder listing (incremental-refresh plan, step 3)

Reviewer: claude-opus-5-5, 2026-10-20. Branch `lens-corrections`, commit `ba961d0` ("Film strip:
list a folder on a background thread"), with `29be4c8` (step 1) and `b69d688` (step 2) read for
context. Design: `docs/ideas/incremental-refresh-plan.md`. No production code or tests were
changed.

## Verdict

**The threading and ordering contracts hold. Fix B1 before shipping, and decide on B2.**
- Thread lifetime, the two generation numbers, cancellation, stale-batch dropping and append
  ordering are correct. Each one is either tested or follows from the code by construction.
- **B1** is a real wrong-target bug for the culling keys. It is small to fix.
- **B2** is a measured miss of the plan's own stall target on two plausible tree shapes. The
  commit message's numbers hold only for the benchmark's shape.
- Everything else is a risk that a later step already owns, or a nit.

## Findings

### Bugs

**B1. Opening a file that the listing has not reached yet leaves the previous shot active and
selected.** `src/app/ui/FilmStrip.cpp:658-664`, reached from `src/app/ui/MainWindow.cpp:1685-1710`.
- When `openFile` finds the file "coming", it skips `setFolder` and calls
  `filmStrip_->setActive(path)`. `rowOf(path) < 0`, so `setActive` stores `pendingActive_` and
  returns early. `active_`, the delegate's highlight and the selection still point at the shot that
  was active before.
- Scenario:
  1. Open a large tree with subfolders on.
  2. Click shot X while the walk runs.
  3. Use File > Open on photo Y in a subfolder the walk has not reached yet.
  4. Y is developed, but X stays active and selected. `targets()` returns X and `activeMarks()`
     shows X's marks, so a rating or label key writes **X's** sidecar (`MainWindow::writeMarks`
     goes to `writeSidecarMarks(X)`).
- This lasts until Y arrives. If Y never arrives (a hidden folder, or a file created after its
  folder was read), it lasts until the next folder change. `activeChanged` is not emitted either.
- Fix: in the pending branch, clear the active shot first. That means `active_.clear()`,
  `delegate_->setActive({})`, clearing the selection and `emit activeChanged()`, then storing the
  pending path. In effect it is `clearActive()` followed by `pendingActive_ = primary`. Add a widget
  test for this sequence: active X, then a pending Y, then press a rating key.

**B2. One large flat folder, or many tiny folders, still freeze the GUI for 130-300 ms.** Measured
with the step's own benchmark (Release, `ARRAW_BENCH_TREE`), over 3 runs each:

| Tree | Inserts | Longest GUI stall while listing | First insert |
|---|---|---|---|
| Generated, 200 × 250 shots (the bench's shape) | 200 | 14.8 / 27.0 / 26.7 ms | 6.6 ms |
| **One folder, 50 000 ARW+JPG** | 1 | **184 / 142 / 129 ms** | 605 ms |
| **20 000 folders × 2 shots** | 20 000 | **298 / 207 / 269 ms** | 9.8 ms |
| 20 000 empty folders, 1 shot | 1 | 24 / 17 / 17 ms | 101 ms |

- The flat folder: a directory is one `FolderResult` (`src/app/FolderLister.cpp:211-222`), so
  50 000 rows arrive as one `beginInsertRows`. That one insert pays for the vector insert, the
  reindex, the proxy mapping, `addShots` and 100 000 loader jobs in a single GUI turn.
- Tiny folders: `applyFolders` emits one insert per new folder (`ShotModel.cpp:423-434`,
  `464-483`). `QSortFilterProxyModel` does O(N) work per source insert, so a batch of about 256
  tiny folders costs about 256 × O(N). The stall is much longer than one batch should take, so
  several batches were most likely applied in one turn. Qt delivers every posted event in the
  queue before the timer gets its turn, and the lister posts without waiting
  (`FolderWatcher.cpp:177-182`). Once the GUI falls behind, the backlog becomes one stall. This is
  an inference: I could not profile (no `perf`, and ptrace is restricted). The empty-folder run
  rules out the end of the walk (sweep and rewatch, about 20 ms) as the main cause.
- All numbers are without a `QListView` or `FilmStrip` handlers attached, so the real strip is
  slower.
- Options, which can be combined:
  - (a) During a first listing every new span is appended. Coalesce the batch's new folders into
    **one** `beginInsertRows` per batch.
  - (b) Cap a `FolderResult` at about 2 000 shots. The lister splits a big directory into chunks,
    and the model appends chunks of a span whose folder is the last one.
  - (c) Flow control: the lister posts the next batch only after the GUI has acknowledged the
    previous one (an atomic "in flight" flag). Batches then grow instead of piling up.
  - (d) The GUI applies a batch within a time budget and re-posts the rest.
- (a) and (c) are small. (b) changes the span invariant only for the first listing.

### Risks

**R1. The end-of-walk sweep treats every failed read as "gone".** `ShotModel.cpp:393-407`, with
`FolderLister.cpp:223-226` (the error becomes a bare count) and `FolderWatcher.cpp:241,245-276`
(the watches follow the summary).
- On a refresh walk, an `EIO`, `ENOTCONN` or `ESTALE` on a subfolder removes its whole subtree.
  An unreadable root (NAS asleep, unmount) removes every row. The rewatch then drops those
  folders' watches, so no event will bring them back when the share returns. Only a new folder
  open, or F5 once step 5 lands, recovers.
- A permission flip (EACCES) removes the subtree. That matches open question 2. A vanishing folder
  is removed correctly.
- This is the same behaviour as step 2, and step 6 owns the fix. What makes it more exposed now is
  that every sidecar write triggers a full-tree re-walk until step 4. So one NAS hiccup during a
  rating session empties the strip.
- Cheap preparation: carry `{path, error_code}` of the failed directories in `ListingSummary`,
  instead of just `unreadable`. Then the sweep can skip the subtrees under a transient failure,
  and step 6 has its data.

**R2. `activateFirstListed_` outlives its listing.** `MainWindow.cpp:1743, 1803-1811`;
`FilmStrip.cpp:575`.
- `firstShotListed` is sent only while listing, and only once a shot passes the proxy. Open an
  empty folder, or toggle subfolders off with an open photograph and an empty root, and the flag
  stays `true`.
- Later, toggling subfolders **on** while the open photograph is not in the strip (the B3 state)
  opens the first listed shot. That replaces the photograph, or asks Save/Discard.
- Related: on a slow share `openFolder` asks Save/Discard first, then waits for the first batch
  with the old photograph still open and editable. If the user edits it in that wait,
  `activateShot` asks a **second** time and moves them away. That could not happen while the
  listing was synchronous.
- Fix: clear the flag when the listing ends (`listingChanged(false)` without a first shot). Also
  consider not auto-activating if the open session gained unsaved changes after `openFolder`.

**R3. Exit, or a folder change during destruction, waits for the `readdir` under way without a
limit.** `FolderWatcher.cpp:208-214`. The user decided to orphan a hung lister (plan answer 6), but
no step in the plan's table carries that work. The code comment documents the hang. Put it in a
step (4 or 6) so that it is not lost.

**R4. A toggle on the same root keeps its watches, so events during that listing queue a second
full walk.** `FolderWatcher.cpp:195-203, 220-232`, `ShotModel.cpp:343-358`.
- The debounce fires `refresh(dirty)` while `walking_ == Listing`. That sets `refreshWanted_`, and
  a whole re-walk follows the listing. Busy imports pay double. This is harmless for correctness.
- Fix: `startWalk` could leave the debounce alone, and `finishListing` of a *Listing* walk could
  drop a wanted refresh whose only cause was events from before the walk started. That needs a
  timestamp, so it is probably step 4's job.

**R5. The first visible shot can wait for the batch interval.** `FolderLister.cpp:232-235`. Only
the root is flushed at once. When the root has no shots (a typical `Photos/2024/...` tree), the
first shots wait until 512 have gathered or 100 ms have passed. The empty-folder run shows 101 ms.
This is within the plan's target (< 100 ms locally), but the commit's "~7 ms" holds only because
the generated tree fills 512 shots in a few milliseconds. Flushing the first batch that holds any
shot would make it robust.

**R6. The benchmark does not measure what the claim says.** `tests/test_FolderListingBench.cpp:142-194`.
- There is no view, no `FilmStrip` handlers and only the generated uniform shape.
- In runs 2 and 3 the "longest GUI stall" (about 27 ms) is the synchronous `setFolder` reset of
  50 000 rows (`setFolder returned in 27.0 ms`), not the listing.
- The numbers reproduce exactly on this machine. Add the flat and many-tiny shapes, and a
  `FilmStrip` (offscreen) variant, so that B2 regressions show.

**R7. A test depends on timing.** `tests/test_FilmStripListing.cpp:155`
(`CHECK_FALSE(label contains "listing")`). It is checked right after the gate is reached. If
reaching the gate takes ≥ 250 ms (a loaded CI, a sanitizer build), the busy note is already there
and the check fails. Install the gate before `show()`, or assert on the elapsed time
(`listingClock_`) rather than on its absence.

### Nits

- **N1. Test seams in production API.** `FolderLister::setReadHook`, `FolderWatcher::setReadHook`
  and `FilmStrip::setListingHook` (`FilmStrip.h:99`). It is one mutex lock per directory and
  harmless, but `FilmStrip`'s public API now has a test-only member. A `friend` test accessor, or a
  "test seam" note in the brief, would make that explicit.
- **N2. Debug-only span check in one place only.** The spans-tile-rows assertion runs only in
  `applyFolders` (`ShotModel.cpp:379-387`), not after the `finishListing` sweep or `resetFolder`.
  A small `checkSpans()` called at the end of each mutating entry point would cover the plan's
  "one comparator" risk fully.
- **N3. Brief forms.** The type aliases `BatchReceiver`, `FinishReceiver` and `ReadHook`
  (`FolderLister.h:38-47`) have verb briefs ("Receives…", "Is called…"). CLAUDE.md wants noun
  forms for types: "Receiver of a batch…", "Function called on the lister thread…".
- **N4. `finishListing` takes `ListingSummary` and ignores it.** `ShotModel.cpp:393`. That is fine
  for step 5 and 6 plumbing. Say so in the doc ("used from step 5"), or the parameter looks
  forgotten.
- **N5. `shotContaining` is linear.** `FilmStrip.cpp:697-706`, now with N growing during the
  listing. It is called from `openFile` and on every whole-frame render's thumbnail
  (`MainWindow.cpp:2290`). It predates this step, but at 50 000 rows it costs a few milliseconds
  per call. `rowOf` covers the primary case, and an index of companions would cover the rest.
- **N6. `listingReaches` stats each component on the GUI thread.** `MainWindow.cpp:1571-1595`. It
  is bounded by depth, so it is cheap locally but blocks per component on a share. It agrees with
  the walk's `isListedFolder` (symlink and hidden-name rules). It does not check that the file
  forms a shot the listing groups. That only leads to the documented "no active shot" outcome.

## What I verified, and how

- **Suites (Debug, `build/container-debug`).** `arraw-tests`: 1610 cases, all passed.
  `arraw-widget-tests`: 245 cases, all passed. The mask-overlay flake did not occur this time.
  `just format-check` is clean.
- **Determinism.** `[listing]` ran 40 times in the core suite and 15 times in the widget suite
  with no failure, partly while the full suites ran in parallel. No fixed sleeps in the new tests:
  every wait is a condition with a timeout. The old `QTest::qWait(300)` in
  `test_MainWindowOpening.cpp` predates this step.
- **The `test_Progress.cpp:369` SIGABRT is pre-existing and unrelated.**
  - Line 369 is the `TEST_CASE` "An observed loop covers every row once, on every band". Its
    `REQUIRE` at line 381 runs inside `forEachRowBand` on worker threads. Catch2 assertions are
    not thread-safe without `CATCH_CONFIG_EXPERIMENTAL_THREAD_SAFE_ASSERTIONS`, and the tests'
    CMake does not set it.
  - The test dates from `3025a10` (2026-10-06) and runs no listing code.
  - Run alone 300 times with the current binary, it aborted 5 times.
  - Fix: count failures in an atomic inside the band and `REQUIRE` once on the calling thread.
- **Thread ownership and lifetime (read).**
  - The lister is built before `moveToThread`. Its lambdas capture the watcher.
  - The destructor runs `cancel()`, `quit()` and `wait()`, then resets the lister, so the lister
    is deleted only after its thread has stopped.
  - The lister thread's `invokeMethod(this=watcher)` posts during the destructor are removed by
    `~QObject`. Loader posts go to the model, and `firstShotListed` goes to the strip, so every
    queued call is dropped with its receiver.
  - `FilmStrip` declares `model_`, `proxy_` and `watcher_` in that order, so the watcher (and its
    thread) goes before the model it feeds.
  - `ListingGate` is declared after the strip or watcher in every test, and releases in its
    destructor.
- **Generations (read and tests).**
  - `setFolder` bumps both the loader generation and the walk number. `startRefresh` bumps only
    the walk, so the rechecks and loads of a refresh stay valid.
  - Stale batches and stale ends are dropped by the model (`walk != walk_ || walking_ == None`)
    and by `finishWalk`.
  - A toggle is a `setFolder` on the same root, which `test_FolderLister.cpp:148-156` covers.
- **Cancellation (read and tests).**
  - `wanted_` is stored before `begin` is queued, so the walk under way stops at its next check.
  - `chain_` keeps a stale queued step from running a second chain.
  - `current()` is checked after the hook and after the read, so a superseded read is discarded.
  - An abandoned walk is never finished, so no sweep follows a cancelled walk.
  - Refreshes asked for during a walk are merged (`refreshWanted_`, `wantedAll_`,
    `wantedFolders_`) and start at its end, as tested.
- **Ordering (read and tests).**
  - A pre-order walk with children in `folderPrecedes` order gives `FolderOrder` order. Sibling
    subtrees cannot interleave, because the comparison is component-wise with a byte tie-break.
  - During a first listing every insert is an append, as tested. A folder new in a refresh goes
    to `lower_bound`.
  - The selection and current index ride on persistent indexes, and the reset fallback stashes and
    restores them.
  - `thumbnails_.setShots({})` runs before any batch can land, because batches are queued. Each
    insert then feeds `addShots`. Loads are queued per batch, marks before EXIF.
  - `loadingFinished` waits for the end of the walk.
- **Pending activation.**
  - Companions match, and the last `setActive` wins.
  - The pending shot is cleared at the end of the listing, by `clearActive` and by a real
    `setActive`.
  - The subfolder guard counts the pending shot as the active one.
  - A filter that hides the pending shot is cleared when it arrives, as the synchronous code did.
  - `firstShotListed` is queued, guarded by `listingNumber_` and consumed once. It is skipped if a
    shot became active first, which covers a user's click before it lands.
- **Watch set (read and tests).**
  - The watches are the walk's directories, shallowest first and capped at 1000.
  - The diff is taken against `watcher_.directories()`, so a watch Qt dropped is added again.
  - A new root removes every watch at the start of its walk. The same root keeps them.
- **Benchmark (run).** I reproduced the step's numbers and measured the other shapes (B2, R6).

## Could not verify

- Where the time goes in the many-tiny-folder stall. That needs a profiler, so B2's "coalesced
  batches" is an inference from the numbers.
- Stalls with a real `QListView` attached, and on SMB, NFS or Windows.
- A hung `readdir` at exit (R3). I did not simulate one.

## Open questions for the user

1. B2: are 50 000 shots in one folder, or 20 000 near-empty day folders, in scope for "no stall
   over 50 ms"? If yes, which mitigations: (a) one insert per batch, (b) chunked large folders,
   (c) flow control, (d) a time-budgeted apply?
2. R1: should step 3 already keep subtrees whose read failed with a transient error (by carrying
   the errors in `ListingSummary`), or wait for step 6 as planned?
3. R3: which step implements the decided "orphan a hung lister at exit"?
