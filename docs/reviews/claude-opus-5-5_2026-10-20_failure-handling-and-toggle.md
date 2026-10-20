# Review: incremental subfolder toggle, failure handling, 100k note and exit orphaning (incremental-refresh plan, step 6)

Reviewer: claude-opus-5-5, 2026-10-20. Branch `lens-corrections`, commit `28a92a9` (step 6: the
toggle without a reset, "Folder unavailable", unreadable subfolders, the large-folder note, the
bounded wait and orphaning of a hung lister at exit). `5537ef5` (ADR 052, which landed while this
review ran) was read for the docs scan. Design: `docs/ideas/incremental-refresh-plan.md`. Earlier
reviews: `claude-opus-5-5_2026-10-20_background-listing.md` and
`claude-opus-5-5_2026-10-20_per-directory-refresh.md`. No production code or tests were changed.
The probes I wrote live in the session scratchpad. They were built against the debug libraries, not
added to the repository.

## Verdict

**The orphaning is sound, and the toggle's basic paths are correct. Fix B1 before shipping: it
defeats answer 1 in exactly the case the answer was written for. B2 and B3 are small state-machine
holes.**
- The orphaned thread cannot reach the watcher or the model. The `QThread` is never destroyed while
  it runs, and the `QFileSystemWatcher` still dies on its own thread. Each of these is checked below.
- **B1.** When the root goes away, the subfolders that also report lose their rows: an unmount
  reports every watch, and a rename reports the subfolders on their next change. Only the root's own
  rows are kept. I reproduced both with the real lister.
- **B2.** Switching off and then on before the "off" walk ends runs the "on" walk silently. The
  model then says it is not listing. Reproduced.
- **B3.** Toggling before a new folder's first shot arrives means the first shot is never opened.
  Found by reading the code.
- Exit still hangs on a dead share whenever the loader, the thumbnail worker or the export queue is
  inside a read (R2). The orphaning covers the lister only, and ADR 052 does not say so.

## Findings

### Bugs

**B1. A vanished root keeps only its own rows: every subfolder that reports is dropped as gone.**
`src/app/FolderLister.cpp:343-346` (`handleFailure`, `FolderError::Gone`), reached from
`readChanged` (`:534-565`).
- `handleFailure` keeps rows only for `path == root_`. A subfolder whose read fails with ENOENT is
  `dropSubtree`d, whether or not the root is readable.
- **Unmount** (a NAS or USB drive, with the root below the mount point). The kernel sends
  `IN_UNMOUNT` to every watch. Qt drops each watch and reports each path. `readChanged` queues all
  of them, the root first:
  - the root fails and is kept ("Folder unavailable");
  - then every watched subfolder fails with ENOENT and is removed with its subtree.
  - The strip keeps the root's few shots and loses the 50 000 below it, with their marks, EXIF and
    thumbnails. This is the "NAS hiccup" that answer 1 and plan section 6 were written to prevent.
    ADR 052 says "This covers an unmounted drive".
- **Rename** of the root, or of a parent above it. Qt drops only the root's watch. The subfolders'
  inotify watches follow their inodes, so a later file in `moved/a` reports the old path `root/a`.
  That read fails with ENOENT, and `a` is dropped. The strip loses one subtree per change.
- Reproduced with the real lister, debounce 0 ms:
  - Unmount: a tree of `root`, `a` and `b`, renamed away, with `simulateDirectoryChanged(…, true)`
    on all three. 3 rows → 1 row, `directories` 3 → 1, `rootUnavailable` true.
  - Rename: real inotify, a file added in `moved/a`. 2 rows → 1 row.
- Fix:
  - For a `Gone` or `NoPermission` subfolder, probe `root_` first (`directory_iterator(root_, ec)`).
  - If the root does not open, set `rootUnavailable_` and keep the subfolder as stale (push it to
    `summary_.stale` during a walk) instead of dropping it.
  - That one stat per failure is cheap, and it does not depend on the order of the reports.
  - Add the unmount shape as a test (rename the root, then report the root and its subfolders).

**B2. Off, then on before the "off" walk has ended: the "on" walk runs as a silent toggle walk.**
`src/app/ShotModel.cpp:350-363`.
- Off sets `walking_ = Toggle`. On then sees `was != None`, so it neither sets `Listing` nor emits
  `listingChanged(true)`.
- The full walk of the subtree then runs with `isListing() == false`:
  - no busy note, and the empty hint can read "No photographs in this folder" while the subfolders
    stream in;
  - `setActive` of a shot not yet listed stores no pending activation (`FilmStrip.cpp:806-813`
    requires `isListing`);
  - `MainWindow::openFile`'s "a walk is running" test is false;
  - `listingChanged(false)` never comes, so the pending activation is not cleared at the end.
- The window is the time the lister takes to read the root once. That is milliseconds locally,
  but it can be seconds on a share, or forever behind a slow `readdir` from the walk before.
- Reproduced: gate at the root's read on the "off" walk, then `setRecursive(true)`.
  `isListing()` is 0 and `listingChanged` fires 0 times.
- Fix: in `setRecursive(true)`, treat `was == Toggle` like `None` (set `Listing`, emit `true`).
  The end then emits `false`.

**B3. A toggle before a new folder's first shot arrives: the first shot is never opened.**
`src/app/ui/FilmStrip.cpp:766` and `:772-784`.
- `setIncludeSubfolders` sets `firstShotSent_ = true` unconditionally. During a folder's first
  listing, before its first row, that suppresses the `firstShotListed` that `openFolder` armed for
  (`MainWindow.cpp:1756`).
- Switching **off** is worse. The queued lambda finds the proxy empty and emits `listingEnded`,
  which disarms `activateFirstListed_` (`MainWindow.cpp:1848`) while the walk is still running.
- Scenario: open a folder on a slow share, then toggle subfolders either way before the first
  shots show. When they arrive, nothing opens.
- Fix:
  - Set `firstShotSent_` only if it is already set, or if the proxy has rows.
  - In the "off" lambda, emit `listingEnded` only when `!model_.isListing()`.

### Risks

**R1. A recovery merges only the directory that recovered, and the notice clears.**
`FolderLister.cpp:236-240` (success branch). `enter` is `false` for a change read (`:192`, `:563`).
- Take a root that was unavailable during an F5, so none of its subfolders were read, and that then
  recovers through a change report of its own. Its subfolders are not read again, yet
  `rootUnavailable` goes false and the notice disappears.
- Changes made below it in the meantime (the NAS woke with new files) show only after the next
  F5, with no hint of that.
- The same holds for a stale subfolder that recovers through its own report.
- After an unmount and remount (once B1 is fixed), the subfolders' watches are gone (Qt dropped
  them). `budget_` still counts them as watched, so the "not watched" count is wrong too.
- Fix: when a directory that is in `failed_`, or the unavailable root, reads successfully, enqueue
  it again with `enter = true`, so that its subtree is walked and re-watched.

**R2. Exit can still hang on a dead share. A hung lister also blocks every folder opened later.**
`src/app/ShotModel.cpp:193-194` (`~Loader` joins), `src/app/ThumbnailWorker.cpp:160-162`,
`src/app/ExportQueue.cpp:152-153`.
- Only the lister is orphaned. The marks/EXIF loader and the thumbnail worker read files on the
  same share, continuously during a first listing. Their `std::jthread`s are joined
  unconditionally. When the share hangs, one of them is very likely inside a read, so closing the
  window still hangs.
- The plan's other half (section 6: orphan a lister stuck more than 5 s when a new walk is wanted,
  and start a fresh one) is not built. After the lister hangs:
  - opening another folder shows "Listing folder…" for ever;
  - F5 and the toggle are no-ops (`refresh` returns while `walking_ != None`).
- Neither limit is in ADR 052's "Known limits". Either record both there, or extend the bounded
  wait to the other three threads.

**R3. No cancellation check between the watch and the read.** `FolderLister.cpp:201-203` then
`:214-222`.
- `addPath` (`inotify_add_watch`) and the `directory_iterator` probe in `watch()` can block on a dead
  hard mount just as `readdir` does. When the call returns, `step` goes straight on to
  `listFolder(path)` on the same dead path, without checking `current()`.
- The cancelled walk (a new folder, a toggle, or exit) therefore waits for a second timeout.
- `applyMoves` loops over `watch()` for every promoted directory, also without a check.
- Fix: `if (!current()) { abandon(); return; }` after the watch.
- Also, after an orphaning, the orphaned thread may come back while static destructors run at
  process exit. That is a small UB window, and it is worth one sentence in the
  `~FolderWatcher` brief.

**R4. Switching off no longer stops the background work for the rows removed.**
`src/app/ShotModel.cpp:726-741` (`removeRange`), `src/app/ui/FilmStrip.cpp:663-676`.
- The old toggle went through `listFolder`, which calls `thumbnails_.setShots({})`. Now the
  removed rows stay in `ThumbnailWorker`'s `embedded_` and `developed_` queues. Each is still
  decoded and **developed** in the background, and stored in the cache.
- By design the loader is not bumped (plan 5), so its queued marks and EXIF reads for those rows
  also still run. `receive` then discards them.
- Toggling off a 50 000-shot tree right after opening it keeps the CPU busy for a long time on
  shots that are not shown.
- The cached results do help a later toggle on, because `known_` skips them. But the cost is
  unbounded.
- Fix (either): a `ThumbnailWorker::removeShots` on `rowsAboutToBeRemoved`, or a
  generation-scoped prune of the loader's queue for primaries no longer in `index_`.

**R5. The end-of-walk sweep is O(spans × stale) on the GUI thread.**
`src/app/ShotModel.cpp:497-503`.
- `none_of(summary.stale, isWithin)` runs for every span. A share that fails part-way through an
  F5 leaves every folder the walk had not reached as stale, potentially thousands.
- Measured (Debug, model only): 20 000 spans with 2 000 stale folders took **1 888 ms** in one
  `finishListing`. Release is faster, but this is still the longest GUI stall in the feature.
- Fix: `summary.stale` arrives in folder order. Walk it together with `spans_` (a two-pointer
  merge), or put the stale folders in a `std::set<…, FolderOrder>` and check each span's nearest
  stale ancestor.

**R6. A test fixture can be used after it is freed.** `tests/test_FolderLister.cpp:1067-1092`
(`FailingReads` captures `this`), used at `:1118` and `:1197`.
- `FailingReads reads` is declared after the `FolderWatcher`, so it is destroyed **before** the
  lister thread stops.
- Both tests write files into watched directories (`addJpeg(root / "IMG_2.jpg")`,
  `fs::remove(…)`). Each write produces a real inotify report besides the simulated one. A
  second read can therefore run during teardown, and call the hook on a destroyed
  `std::mutex`/`std::set`.
- It passed 25 runs here, but it is a latent crash. Fix: hold the state in a `shared_ptr`, as
  `ListingGate` and the widget tests do, or declare `reads` before the watcher.

### Nits

- **N1. A path in the "Folder unavailable" tooltip can switch it to rich text.**
  `FilmStrip.cpp:614-619`.
  - The tooltip starts with the root's path. `Qt::mightBeRichText` returns true for
    `/photos/<b>2026</b> cannot be read (…)` (checked).
  - The tooltip is then rendered as HTML: the tag is swallowed and the `\n` separators collapse.
  - The unreadable-folders tooltip starts with fixed text, so it is safe today, but only by
    accident.
  - Fix: `Qt::convertFromPlainText` on the joined tooltip, or escape each path.
- **N2. Plurals and system text.**
  - `tr("%1 folders could not be read")` (`:623`) still uses `%1` with a hand-made singular, not
    `tr("%n folder(s)…", nullptr, n)`. The previous review's N2 is still open for these strings.
  - `QString::fromStdString(rootError)` assumes UTF-8. On Windows, `std::system_category()`
    messages come in the ANSI code page.
  - "Click, or press F5, to try again." is repeated in each section of a combined tooltip.
- **N3. The large-folder note.**
  - Its number is frozen. `updateNotice` runs when the note first applies, and after that only on
    a status. A status arrives only when the watch numbers change, so "Large folder: 100,000
    shots" stays while the count reaches 150 000.
  - It is sticky while the count stays above the threshold, rather than "shown once" as ADR 052
    says. That behaviour is fine, but the ADR should match it.
  - Clicking the notice is F5 (`:434`). When the large note is its only entry, a click meant as
    "tell me more" re-walks 100 000 shots and rechecks every sidecar.
- **N4. Dead summary fields.** `ListingSummary::shots` and `::unreadable` are written
  (`FolderLister.cpp:185`, `:247`) but nothing reads them. `shots` also double-counts re-reads
  (the earlier N4). Drop both, or use them for the note.
- **N5. A refused subfolder is tracked anew on every read of its parent**
  (`followSubfolders` → `track` → `budget_.add`).
  - At the cap, that can evict a watched directory, which `dropSubtree` then promotes back with a
    re-read and a sidecar recheck.
  - The result is churn on every parent event. It is harmless, but it is worth a comment, or
    remembering refused paths in the tree.
- **N6. Tests that cannot fail as named.**
  - "A toggle during a walk abandons it, and its late batches are dropped" (`:1294-1326`). The held
    walk is abandoned **before** it delivers anything, so no stale batch ever reaches the model.
    The model's drop of a stale walk is not exercised: that needs a batch queued in `incoming_`
    before the toggle.
  - The exit test's last check (`:1357-1358`) cannot fail short of a crash. It does not check that
    the orphan ended and was deleted, for example with a `QPointer<QThread>` reaching null.
- **N7. More test seams in production:** `setExitWait`, `setLargeFolderShots`, and a read hook that
  may throw. This carries over N5 of the previous review. Their briefs say "seam for tests", which
  is enough.
- **N8. Docs and Doxygen out of date.**
  - ADR 052, "Failure": "This covers an unmounted drive or a NAS going away" is false while B1
    stands. "The 100k note … shown once per listing" does not match the code (N3).
  - `src/app/FolderResult.h:53`: `gone` is "Whether the directory went, **or could not be
    read**". Since step 6, an I/O error keeps the rows; only ENOENT, ENOTDIR and EACCES make a
    folder `gone`.
  - `tests/test_FilmStripSubfolders.cpp:68`: the test name "…toggle **re-lists** the folder…" is
    stale.
  - ADR 029's Consequences still say the watcher "re-lists and diffs" and that a folder "is read
    once and all at once". It is an accepted ADR, so a one-line "superseded in part by ADR 052"
    would do.
  - I found no remaining Doxygen in `src/app` that describes synchronous listing or the old
    `rewatch`. No dead `thread_` or `refresh` re-list code remains.
- **N9. Not in ADR 052, though each is a decision a later reader needs:**
  - the fourth walk kind (`Walking::Toggle`, silent);
  - that the toggle bumps the walk number but not the loader generation, with R4 as its cost;
  - that the orphaning covers the lister only (R2).

### Known (not counted)

- The plan's "orphan a stuck lister on a new generation" is not built (see R2). It was listed as
  "later" in the plan.
- A hard-mounted share can block the lister for ever. The orphaning only stops it holding up exit.

## What I verified, and how

- **Suites (Debug, `build/container-debug`).**
  - `arraw-tests`: 1 643 cases, all passed. `arraw-widget-tests`: 257 cases, all passed. Neither
    known flake occurred. `just format-check` is clean.
  - `[failure],[toggle],[exit]` ran 25 times and `[notice],[subfolders]` (widgets) 10 times, with
    no failure. No new test uses a fixed sleep; every wait is a condition with a timeout.
- **Orphaning (read, plus the exit test and a probe).**
  - `Link::post` checks the target and posts **under the mutex**, and `~FolderWatcher` nulls the
    target under the same mutex, so no post can race the destruction. A post queued before that
    is removed by `~QObject`.
  - The lister's callbacks capture only `shared_ptr<Link>`, never the watcher or the model.
  - `Worker` owns the lister. `release()` means a running `QThread` is never destroyed (Qt 6
    `qFatal`s on that).
  - `finished` → `deleteLater` is queued to the GUI thread. The `isFinished()` fallback can make
    `deleteLater` run twice, which is safe, and `~QThread` waits while `isInFinish`.
  - `shutDown` still runs on the lister thread (`DirectConnection` on `finished`) for an orphan
    too, so `QFileSystemWatcher` and the debounce timer die on their own thread.
  - When a held hook returns, `current()` is false (`wanted_ == 0`), so the thread runs `abandon()`
    and delivers nothing. `quit()` issued during a handler takes effect when control returns to the
    loop.
- **Toggle.**
  - It bumps `walk_` but not `generation_`. `incoming_`, `nextFolder_` and `nextShot_` are
    cleared, so a folder that was partly inserted is merged cleanly by the next read of it.
  - Off is one `removeRange` (`spans_.upper_bound(root)` up to the end; the root sorts first). The
    lister's later `gone` entries for the same folders are no-ops.
  - The active shot is cleared before the removal when it leaves, and a pending activation in a
    subfolder is dropped.
  - Watches are released (tested). On is one more read of the root, then each subfolder once
    (tested).
  - F5 during a toggle walk is folded into it (`refresh` returns while `walking_ != None`). A
    toggle during a refresh keeps `Refresh` and sweeps at its end.
  - The Save/Discard guard runs in a modal `exec`, and `setIncludeSubfolders` recomputes "leaving"
    after it.
  - Rapid on → off is correct (it stays `Listing`). Off → on is B2.
- **Stale and sweep.**
  - Rows at and below a stale folder survive the sweep (tested).
  - A recovered folder merges into its existing span, so its rows are not duplicated.
  - Failures are forgotten when the folder is dropped, when it vanishes from its parent's listing,
    when the root changes, and when subfolders are switched off (`forgetVanishedFailures` with an
    empty list).
  - EACCES flip and restore through the parent's report (tested with a simulated report). Under
    real inotify, a `chmod` on a child should give the parent an `IN_ATTRIB` report. That is from
    memory of Qt's watch mask; I did not run it.
  - A root that goes unavailable during F5 sweeps nothing (tested). Recovery by F5 and by the
    root's own report both work (tested), but see R1.
- **Notice.**
  - The priority order is unavailable, then unreadable, then unwatched, then large, joined with
    " · ". The tooltip lists the first five paths and "… and N more". It clears on a new folder
    (`watchStatus_ = {}`), and on a status of 0 after a toggle off or a successful refresh.
    Statuses of stale walks are dropped.
- **100k note.** It counts rows (shots), not `summary.shots` (which double-counts). It resets on a
  folder change (`largeNoted_ = false` in `listFolder`) and blocks nothing.

## Could not verify

- Real unmount and remount of a CIFS or NFS share, and a real hung hard mount. B1 was shown with a
  rename plus simulated reports, which match the sequence Qt's inotify engine produces for
  `IN_UNMOUNT`.
- How Qt behaves when the orphaned thread returns after `QCoreApplication` has been destroyed
  (R3's UB window).
- Release timing of R5. Only Debug was measured.

## Open questions for the user

1. B1: is "probe the root before dropping a subfolder as gone" the behaviour you want? The
   alternative is "while the root is unavailable, drop nothing". It is simpler, but a subfolder
   that really was deleted during a rename would then linger until F5.
2. R2: should the bounded wait at exit extend to the loader, the thumbnail worker and the export
   queue? The export queue may be writing a file. Or should the ADR just record the limit? Is the
   "fresh lister after 5 s" half of answer 6 still wanted?
3. R4: should switching subfolders off cancel the thumbnails and EXIF still queued for the
   subfolders' shots, at the cost of re-queueing them when switched on again?
4. N3: should a click on the notice when it shows only the large-folder note do nothing, or
   should that note be a plain label?
