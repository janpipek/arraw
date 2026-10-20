# Review: per-directory refresh, watch budget and Refresh Folder (incremental-refresh plan, steps 4 and 5)

Reviewer: claude-opus-5-5, 2026-10-20. Branch `lens-corrections`, commits `5ad03d2` (step 4: the
lister thread owns the watcher, each directory is watched before it is read, a change re-reads only
its directory) and `65beb2f` (step 5: `WatchBudget`, the "not watched" notice, Refresh Folder/F5,
the hidden `filmStrip/maxWatchedFolders`). Design: `docs/ideas/incremental-refresh-plan.md`.
Previous review: `claude-opus-5-5_2026-10-20_background-listing.md` (its bugs were fixed in
`5c0ae1f`). No production code or tests were changed. The probes I wrote live in the session
scratchpad, not in the repository.

## Verdict

**The threading contracts and the watch-before-read rules hold. Fix B1 before shipping. R1 and R2
are cheap to fix and worth doing in the same change.**
- The watcher is created, used and destroyed on the lister thread, and shutdown is ordered
  correctly. Watch-before-read, the re-read after a watch is gained, and the "Qt dropped the watch"
  check all follow the plan's rules 1 to 5. Each is either tested or follows from the code.
- **B1.** Step 4 made a first listing no longer append-only. `ShotModel::insertRun` still
  coalesces new folders as though each batch held distinct folders in folder order. I reproduced
  duplicate rows and out-of-order spans end to end (debug assertion). In production the 300 ms
  debounce makes it rare, but not impossible.
- `WatchBudget` is deterministic and correct for its main job. Its failure paths have two holes
  (R3, R4).
- The benchmark numbers reproduce exactly. The commit message's "13 ms" leaves out the 300 ms
  debounce (N1).

## Findings

### Bugs

**B1. A batch that holds one folder twice, or a new folder earlier in order than the one before
it, corrupts the rows.** `src/app/ShotModel.cpp:543-560` (`insertRun`), fed by
`src/app/FolderLister.cpp:224-228, 446-449`.
- `insertRun` puts consecutive *new* folders into one insert when `spans_.lower_bound(folder)`
  gives the same place. `spans_` is updated only after the loop (`:586-593`). So within one run:
  - a folder that appears **twice** is not found the second time, and is inserted twice;
  - a folder that sorts **before** the previous piece but has the same `lower_bound` (both are
    "end") is appended **after** it, although its key sorts first. The span map and the rows then
    disagree.
- The lister can now produce both. During a first listing, `batch_` collects every directory read
  in the last ≤ 100 ms (`batchInterval`). That includes change re-reads, which `readChanged`
  pushes to the front of the queue:
  - **Duplicate:** directory D is read by the walk, D reports a change, and the debounce fires
    before the next flush. The batch is then `[…, D, …, D]`.
  - **Misorder:** the walk reads `2026/10-21` (new, so `lower_bound` is end). Then the parent
    `2026` reports the new `2026/10-20` and is read again. The parent has no shots, so it has no
    span, and `insertRun` skips it without breaking the run (`:551-555`). Then `10-20` is read. Its
    `lower_bound` is also end, so it is appended after `10-21`.
- Reproduced end to end with the real lister and inotify, debounce 0 ms:
  - Writing `a/IMG_2.jpg` while `a` is read, with three slower siblings after it, aborts on
    `checkSpans` (`span.first == next`).
  - Creating `bb/` (between `b` and `c`, with the root holding no shots) while `c` is read aborts
    the same way.
  - Feeding the model `[root, 2026, 2026/10-19]` and then `[2026/10-21, 2026, 2026/10-20]`
    directly reproduces the misorder deterministically.
- In Release there is no assertion. The rows stay duplicated or misordered, `index_` points at
  one of the copies, and later diffs, `removeTree` and `rowOf` work on the wrong ranges.
- Production uses a 300 ms debounce, which is longer than the 100 ms batch window, so the usual
  path is safe. Two paths remain:
  - `maxLatency` (`FolderLister.cpp:412-415`) can start the timer at about 0 ms, when another
    directory has been dirty for about 2 s (a long import during the walk).
  - A promotion re-read (`applyMoves` → `enqueue(…, recheck)`) can land in the same batch as
    the walk's read when a transient refusal leaves no `gone` entry in between (see R4).
- I did not reproduce it with the 300 ms debounce (three runs of an import plus writes ahead of
  the walk). The tests construct `FolderWatcher` with 0 ms and 20 ms, so the suite can hit it.
- Fix, either or both:
  - (a) In `insertRun`, break the run unless `folder` strictly follows the previous piece in
    `FolderOrder`. That one comparison catches both shapes.
  - (b) In `FolderLister::step`, when `batch_` already holds `path`, flush first.
- Add a model test with the two batches above.

### Risks

**R1. `QKeySequence::Refresh` is bound only by its first key.** `src/app/ui/MainWindow.cpp:226`.
- `setShortcut(QKeySequence::Refresh)` converts the enum through `QKeySequence(StandardKey)`,
  which keeps only the **first** binding. The wiring rule is to use the platform's bindings
  (`setShortcuts`), as Undo and Redo do.
- Verified here (offscreen): `keyBindings(Refresh)` is `[F5, Refresh]`, and the action gets F5
  only, so the keyboard's Refresh media key is lost.
- On GNOME, Qt documents Refresh as `Ctrl+R, F5`. If that order holds, the action gets `Ctrl+R`
  and **F5 does nothing**. On macOS it is `Shift+Cmd+R`. `docs/keybindings.md:32` promises F5
  everywhere. I could not check either platform's order here.
- The test (`test_MainWindowView.cpp`, "F5 refreshes…") compares `shortcut()` against the same
  first key, so it cannot catch this.
- Fix: `setShortcuts(QKeySequence::Refresh)`. Have the tooltip and the doc row list the
  platform's keys.

**R2. A watch that Qt drops can be forgotten for good.** `FolderLister.cpp:399-402`
(`onDirectoryChanged` returns when `!current()`), `:109-121` (`begin` keeps `dir.watched`), and
`:185` (step re-watches only `!dir.watched`).
- Between `start()` storing `wanted_` and `begin()` running on the lister thread, `current()` is
  false, and the report is **dropped**, not added to `dirty_`.
- If that report was Qt's "watch removed" for directory D (`IN_DELETE_SELF` or `IN_MOVE_SELF` on
  a directory that was replaced), `dir.watched` stays true. Qt no longer watches D, and nothing
  ever checks it again:
  - The presence check in `readChanged` runs only for dirty directories.
  - F5 (a walk of the same root) trusts `dir.watched`.
  - D's later changes are missed, and the status still counts D as watched. Only opening another
    folder recovers.
- The window is small, because `begin` is posted before the next poll. But a dropped report is
  the one event the plan's rule 5 relies on.
- Fix (small):
  - Record reports whatever `current()` says. `readChanged` already keeps `dirty_` for the next
    walk, and `begin` clears it for another root.
  - At the start of a same-root walk, reconcile `dir.watched` with `watcher_->directories()`
    once, so that F5 also repairs a watch that was lost.

**R3. One directory that is refused a watch for good costs a place each time a place frees.**
`src/app/WatchBudget.cpp:69-81, 103-111`, `FolderLister.cpp:324-332`.
- `refused()` moves the directory to `waiting_` and sets `cap_ = watched_.size()`. When a watched
  directory goes, `promote()` gives the freed place to `waiting_.begin()`. That is the refused
  directory again if it is the shallowest. Its retry is refused, and the cap drops by one more. The
  freed place never reaches the next waiting directory.
- For ENOSPC this is right, because the system really is full. For a refusal of one directory
  only, it is wrong:
  - Windows `FindFirstChangeNotification` on a share or folder that does not support it.
  - A mount on Linux whose `inotify_add_watch` fails while `opendir` works.
- In those cases each folder removal lowers the cap until the next walk. `setCap` at a walk's
  start does restore it, so this is not permanent across F5.
- The transient case asked about: a single transient refusal shrinks the cap only until the next
  walk (new folder, toggle or F5). Nothing retries it in between, and the notice tells the user to
  press F5. That is acceptable, but it is not stated in the tooltip.
- Also, every refusal during a walk costs an extra `opendir` probe on the lister thread. Qt logs
  an `inotify_add_watch … No space left on device` warning per attempt.
- Options:
  - (a) Remember refused paths, and skip them in `promote()` until `setCap`.
  - (b) On a refusal, shrink only when the probe succeeds **and** the error is ENOSPC. Qt does
    not expose errno, so this would need a `statfs`/`inotify` check, which is probably not worth
    it.
  - (c) Accept, and document.

**R4. A failed watch probe leaves a directory in the tree but out of the budget.**
`FolderLister.cpp:330-331`.
- When `addPath` fails and `directory_iterator` fails too, the directory is removed from the
  budget. When the following `listFolder` then **succeeds** (the directory was replaced between
  the two, or a share hiccup), it stays in `tree_` with no budget entry.
- `track()` never adds a known path again, so the directory is never promoted or watched until
  the root changes. F5 does not help.
- Fix: re-add it to the budget (`budget_.add(path, depth)`) when its read succeeds, or remove it
  from the budget only in `dropSubtree`.

**R5. A vanished root or mount tries a watch on every waiting directory first.**
`FolderLister.cpp:185-189, 330-331, 348-361`.
- When the root goes (unmount, NAS asleep), each watched directory reports and is re-read. The
  root comes first. Its watch fails and its probe fails, so `budget_.remove(root)` promotes the
  next waiting directory. That one's watch and probe fail too, which promotes the next, and so on
  through **all** of `waiting_`: up to about 49 000 `inotify_add_watch` and `opendir` calls before
  the root's read fails and drops everything.
- Locally that is milliseconds. On a soft-mounted share, each probe can wait for a timeout.
- The outcome (rows dropped) is step 6's (see Known). The cascade is new with step 5.
- Fix: do not promote while handling a refusal of a directory that does not open, or stop the
  cascade after the first failure under the same failed ancestor.

**R6. Refresh gives no feedback, and repeats.** `src/app/ui/FilmStrip.cpp:583-587`,
`ShotModel.cpp:343-351`.
- The busy note shows only while *listing*. A refresh of 50 000 shots takes about 2.3 s with no
  visible sign, so a second click or F5 is likely. Each press during a refresh queues one more
  full walk.
- F5 during the **first** listing queues a full walk after it, which doubles the time on a share.
  The listing already reads everything.
- Options:
  - (a) Show the busy note during a refresh too.
  - (b) Make F5 a no-op while listing, and fold presses during a refresh into the one under way.
  - (c) Disable the action and the notice while `isRefreshing()`.

### Nits

- **N1. The benchmark claim leaves out the debounce.** The step-4 message says "a new file shows
  … about 13 ms after it appears". The bench runs with a 0 ms debounce
  (`test_FolderListingBench.cpp:207`). With the production 300 ms debounce, a user sees about
  315 ms. The 2 s re-walk it is compared with also waited for the debounce. The number itself
  reproduces exactly (median 12.5 / 12.7 / 12.8 ms, refresh of all 2354 / 2318 / 2320 ms).
  Reword it as "13 ms of work after the debounce".
- **N2. Plural and platform text.**
  - `tr("%1 folders not watched")` (`FilmStrip.cpp:600-601`) should be `tr("%n folder(s) not
    watched", nullptr, n)` for translation.
  - The tooltip says "inotify on Linux" on every platform.
  - On macOS, Qt's FSEvents engine has no such limit, so the 1000 cap there is self-imposed.
- **N3. `firstDirty_` is per dirty set, not per directory**, as the brief of `maxLatency`
  (`FolderLister.h:96-98`) says. A directory that first reports at 1.9 s is read about 100 ms
  later, without its own debounce. That is harmless, apart from feeding B1.
- **N4. `summary_.shots` counts a directory twice** when it is re-read for a change during a walk
  (`FolderLister.cpp:220-222`). That matters once step 6 shows the count in the 100k note.
- **N5. Test seams in production API** (carried over from N1 of the step-3 review): `setWatchGate`
  and `simulateDirectoryChanged` join `setReadHook`. Each costs one mutex lock per watch or per
  read. The briefs say "seam for tests", which is enough.
- **N6. Three names for one constant:** `defaultMaxWatchedFolders`,
  `FolderLister::maxWatchedFolders` and `FolderWatcher::maxWatchedFolders`. There is also an empty
  `namespace {}` at `WatchBudget.cpp:13`.
- **N7. A promotion re-read rechecks every sidecar of its folder** (`recheck = true`,
  `FolderLister.cpp:358`). Deleting a subtree that held 500 watched folders promotes 500 others,
  and their 500 reads stat about 125 000 sidecars in the background. This is defensible (a sidecar
  could have been replaced while the folder was unwatched), but it is worth a comment.
- **N8. Test gaps.**
  - No widget test that F5 keeps the selection and the active shot (plan section 7, "F5 merges
    and keeps the selection"). The model's merge path guarantees it, but the reset fallback above
    64 runs is the risky case.
  - No test of a change during a first listing that lands in the same batch (B1).

### Known / step 6 (not counted as bugs)

- A failed read is treated as gone: `FolderLister.cpp:207-218` drops the subtree on any error,
  and the root's failure drops every row (answers 1 and 2). R5 makes that path more expensive.
- The "Folder unavailable" and "could not be read" notice texts. `WatchStatus::unreadable` is
  carried but not shown.
- The incremental toggle. A toggle is still a `setFolder` with a reset. The lister keeps the same
  root's tree, so toggling off emits one `gone` per child into an empty model, which is harmless.
- The 100k note.
- Orphaning a hung lister at exit. `~FolderWatcher` still waits for the `readdir` under way.

## What I verified, and how

- **Suites (Debug, `build/container-debug`).** `arraw-tests`: 1631 cases, all passed.
  `arraw-widget-tests`: 252 cases, all passed. Neither known flake occurred. `just format-check`
  is clean.
- **Determinism.** `[watcher],[budget],[shotmodel]` ran 40 times, and
  `[notice],[filmstrip]` with the F5 window test ran 25 times, with no failure. No new test uses a
  fixed sleep: every wait is a condition with a timeout.
  - Absence is proved by a sibling event: `a2.jpg` beside an unwatched `sub3`, `a/IMG_2` beside
    unwatched `c` and `a/x`, and an ignored report followed by a counted one.
  - The refusal test's gate counts calls, so it is deterministic.
- **Thread affinity and shutdown (read).**
  - `QFileSystemWatcher` and the debounce `QTimer` are made lazily in `begin` on the lister
    thread (`ensureWatcher`). They are used only there. Other threads read the
    mutex-guarded `watchedCopy_`.
  - They are destroyed in `shutDown`, which is connected to `QThread::finished` with
    `Qt::DirectConnection`. That signal runs on the lister thread after its event loop has
    stopped, so the inotify socket notifier, or the Windows engine's handle threads, are torn
    down on their own thread.
  - `~FolderWatcher` cancels, quits and waits, and only then resets the lister. Posts to the
    watcher are removed by its `~QObject`.
  - No signal can arrive after the thread ends: the engine is gone by then.
- **Watch, then read (read and tests).**
  - `step` watches before the hook and the read (`:185-206`). The test holds the walk at `c`'s
    read and finds `c` already watched.
  - A watch gained later (promotion, or a watch Qt dropped and the directory reported) is
    followed by `enqueue(…, recheck)`. The "gains a watch" test shows `c/IMG_9` written while
    unwatched.
  - A new subfolder is watched before its first read. The 20 × `new/dN/x` R7 loop passes.
  - Events during a walk for a directory still queued fold into its read (`enqueue` returns
    early). Events for one already read go to `dirty_` and are read ahead of the walk, in folder
    order.
- **Replace and rename (reasoned from Qt's inotify engine).**
  - Qt drops the watch on `IN_DELETE_SELF` or `IN_MOVE_SELF` and reports the path.
  - `readChanged` sees the path missing from `directories()` and clears `dir.watched`, so the
    re-read watches it again. A replaced directory gets a new watch.
  - A renamed one is gone from its parent's read (`dropSubtree` releases the old subtree's
    watch descriptors first). The new name is walked as new, and its watches are added after
    the old ones are removed, so inotify's per-inode descriptors do not collide.
  - The marks, EXIF and thumbnails of a renamed subtree are re-loaded, as for any new folder.
- **WatchBudget (read and tests).**
  - Shallowest first, ordered by depth and then `folderPrecedes`. The result does not depend on
    the order of adding, which is tested with three orders.
  - Eviction gives the place to a shallower directory. `remove` promotes the shallowest
    waiting directory.
  - `setCap` restores the cap at each walk, and the refused directories are tried again.
  - `refused` shrinks the cap by one per refusal. During a walk, `watched_` also counts places
    granted but not yet watched, so a run of ENOSPC refusals converges on what the system gave.
    That is correct, at one probe per refusal (R3).
- **The model under change results (read and tests).**
  - Batches of the current walk are applied after its end, and an end is applied once
    (`ShotModel.cpp:412`).
  - Stale walks are dropped. A refresh started while change results are still queued drops
    them, and its own walk reads those directories again.
  - `gone` means `removeTree`: one contiguous removal, and `insertRun` breaks the run before it.
  - The end-of-walk sweep runs for a refresh only. Every directory that a same-root walk reads
    is marked seen, change re-reads included, so the sweep removes only folders the walk did not
    find.
  - Rechecks happen only for `recheck`, for a changed diff, or during a refresh of all. The
    tests count the loader jobs.
- **The refresh of all.** It is a merge, not a reset, so the selection, the current index and the
  active shot ride on persistent indexes. The reset fallback stashes and restores them. Every
  loaded row's sidecar is rechecked. Measured: 50 020 shots refresh in about 2.3 s
  (Release, best of 3, three runs).
- **Debounce.** The 300 ms trailing debounce is capped at 2 s after the first report
  (`maxLatency`, answer 3), on the lister thread.
- **Notice and action.**
  - The notice hides at 0 unwatched, clears on a new folder or a toggle (`listFolder` resets
    `watchStatus_`), and updates when a folder goes. Statuses of stale walks are dropped.
  - Refresh Folder starts disabled and is enabled by `folderChanged`. It is also on the window,
    so it works with the menu bar hidden.
  - The strip's context menu has it on a cell, and on empty space when a folder is shown.
  - F5 and Ctrl+R collide with no other shortcut in `src/app/ui` or `docs/keybindings.md`. `R`
    without a modifier is the red label.
- **The hidden setting.** It is clamped to [1, 1 000 000]. A value that is not a number gives
  the default. It is read once at start-up and applied from the next walk.

## Could not verify

- `QKeySequence::Refresh`'s binding order on GNOME and macOS (R1). The offscreen platform ignores
  the platform theme.
- Qt's FSEvents engine on macOS. From memory of Qt's implementation, each `addPath` restarts the
  stream with the whole path list, which would make 1000 watches added one at a time cost
  O(n²). That is on the lister thread, not the GUI thread, but worth a measurement.
- Windows: `FindFirstChangeNotification` handles stop the user from renaming a watched folder's
  parent in Explorer. That predates this work (the old watcher also held up to 1000), but it is
  more visible now that the watches follow the tree.
- B1 with the production 300 ms debounce. I reproduced it only with debounces shorter than the
  100 ms batch window. The `maxLatency` path makes it possible, but I did not hit it in three
  runs.

## Open questions for the user

1. B1: fix it in the model (refuse to coalesce out of order, which is robust whatever the lister
   sends), in the lister (flush before a folder repeats), or both?
2. R3: should a directory the system refuses be skipped by promotion until the next walk, or is a
   cap that shrinks per refusal acceptable?
3. R6: during a refresh, should F5 show the busy note, be disabled, or fold into the refresh under
   way? And should F5 during a first listing do nothing?
