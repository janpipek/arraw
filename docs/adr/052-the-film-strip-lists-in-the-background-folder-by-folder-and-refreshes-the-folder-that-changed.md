# The film strip lists in the background, folder by folder, and refreshes the folder that changed

Status: accepted, 2026-10-20.

With "Include subfolders" on, opening a tree froze the window for the whole walk, and every
debounced watcher event listed the whole tree again, diffed it quadratically and walked it a
third time to re-watch it (review `claude-opus-5-5_2026-10-19_film-strip-subfolders-badge.md`,
R1, R2, R3, R7, B1). At 50 000 shots that is about 0.7-0.9 s per event on a warm local disk, and
tens of seconds on SMB or NFS. The plan is `docs/ideas/incremental-refresh-plan.md`; this ADR
records what was built (commits 29be4c8 to 28a92a9, and the fixes of the step-6 review) and the
decisions behind it. The user decided:

- **No cap on shots, none on depth.** A photographer's archive is as big as it is. A listing past
  100 000 shots shows a note in the strip; it blocks nothing.
- **Incremental and asynchronous.** A background listing on opening, a refresh per folder after.
- **The cap on watches stays, at 1000 directories, but is visible**: a notice says how many
  folders are not watched, and there is a manual refresh key.
- The nine open questions of the plan were all answered as recommended (below).

Decisions made while building follow from these.

## Listing

- **The core reads one folder at a time.** `listFolder(dir)` returns that directory's shots and
  its listable subfolders from one `readdir`, each entry stat'ed once; `folderPrecedes` is the
  shared folder order (natural, component by component); `FolderError` classifies why a read
  failed. `listShots` is a pre-order walk over `listFolder`, with the result it always had, except
  that sibling folders equal in natural order keep their subtrees together.
- **A `FolderLister` walks on a thread of its own**, one directory per turn of its event loop,
  owned by `FolderWatcher`. The walk is in strip order, so a first listing only appends rows.
  Results go out in batches of `FolderResult`: at once when the first shots are found, then every
  512 shots or 100 ms, and at the end. Every result carries the number of its walk; starting a
  walk abandons the one under way, and nothing of the old one is delivered or reported finished.
- **The model applies under a time budget.** Queued results are applied about 12 ms per event-loop
  turn; inserts are coalesced and capped at 2000 rows. The root is still checked synchronously on
  opening, with its error dialog (answer 4). The strip shows a listing note while the walk runs,
  and a photo opened before the walk reaches it becomes active when it arrives.
- **Rows are kept as one span per folder** in `ShotModel`. A merge diffs each folder's span against
  its fresh listing (`FolderDiff.h`), linear in the folder, and signals one insert or remove per
  run, falling back to one reset past 64 runs; the strip stashes and restores its selection around
  a reset. Folders in one batch merge into a single insert only when each strictly follows the
  last, so a folder re-read mid-listing cannot duplicate or misorder rows.
- **Sidecars are rechecked only for the folder that changed** (or the folders the watcher named),
  not for every loaded shot.
- **The include-subfolders toggle is incremental**: it neither resets the model nor re-walks the
  tree. Turning it on lists the subfolders (a listing, with its note, also when it replaces a
  switch off whose walk had not ended); turning it off drops their spans with one signal, and
  the marks, EXIF and thumbnails still queued for them, which switching on queues again. The
  walk of a switch off reads the root only to release the subfolders' watches, and is a fourth
  kind of walk, silent (`Walking::Toggle`). The toggle starts a new walk number, so batches of
  the walk under way are dropped, but not a new loader generation, so the reads owed to the rows
  that stay still land.

## Refresh and watching

- **A change re-reads only its directory.** The lister owns the watcher, the dirty set and the
  debounce (300 ms, but no later than 2 s after the first report, so a long import is shown about
  every 2 s: answer 3). A new subfolder is walked with its subtree; a vanished one is dropped in
  one signal with its subtree and its watches.
- **Watch, then read.** Each directory is watched before it is read, so a change after the read is
  reported and one before is in the read. A directory that got its watch only after its read (the
  system refused, or Qt dropped it) is read once more; a change report that arrives before a walk
  starts is kept.
- **Watches go to the shallowest folders first** (`WatchBudget`: shallowest, then
  `folderPrecedes` order), at most 1000. A shallower folder that appears takes the place of a
  deeper one; a place freed is promoted to the next waiting folder, which is read once more. When
  the system refuses a watch (the inotify limit is shared with other programs) the cap shrinks to
  what it accepted until the next walk; a refused folder does not get freed places in between.
- **A title-bar button says "N folders not watched".** It, and View > Refresh Folder (also in the
  strip's context menu), re-read the folder in the background (answer 8). The key is F5 on every
  platform plus the platform's own Refresh bindings (`docs/keybindings.md`). The strip shows
  "refreshing..." and coalesces repeats. Unwatched folders are not polled (answer 9).
- **The cap is a hidden setting**, `filmStrip/maxWatchedFolders` (default 1000, clamped to
  1..1 000 000), for those who raised the system's limit (answer 7). No UI.

## Failure

- **A root that cannot be read keeps its rows.** The strip says "Folder unavailable", reads no
  subfolder, sweeps nothing, and merges normally again on the next event or F5 (answer 1).
- **A subfolder that is gone is dropped with its subtree. One refused for lack of permission is
  dropped too** and counted as unreadable until a read of its parent finds it readable (answer 2).
  **One with any other error** (an I/O error on a share) keeps its rows, is counted and listed
  stale, and is read again at its next report or refresh.
- **Before a subfolder is dropped, the root is opened.** If it does not open, the root is
  unavailable and the subfolder is kept as stale, like the root's own rows. This is what covers
  an unmounted drive or a NAS going away: an unmount reports every watch, and a rename of the
  root (or above it) reports the subfolders under their old paths, each of which fails as gone.
  The user chose this over "drop nothing while the root is unavailable", which would keep a
  subfolder really deleted meanwhile until F5.
- **A directory that reads again after it failed** (the root, or a stale subfolder) has its
  subtree walked again and re-watched, with its sidecars checked; the notice clears only once all
  of it is read. A subfolder that reads while the root is unavailable (renamed away and back) has
  the root read too. After an unmount Qt has dropped every watch, so nothing reports the remount:
  F5 brings the tree back.
- **A lister stuck in `readdir` on a hung share does not hold up exit.** The destructor waits at
  most `FolderWatcher::defaultExitWait` (2 s), then orphans the thread: it owns the lister and
  everything it touches, delivers nowhere, and deletes itself when the call returns (answer 6).
  Only the lister is orphaned (see Known limits).
- **The 100k note counts shots.** It turns on once per listing and stays while the strip holds
  that many (answer 5). Its count is the one when it turned on, and is updated only with the rest
  of the notice. On its own it is a plain label: a click on it would otherwise re-walk 100 000
  shots.

## Smaller decisions

- **The remembered folder is the strip's folder**, not the opened file's directory: with subfolders
  on, opening a photo from below the strip's folder restores the strip's folder and then the photo
  (`rememberFolder(filmStrip_->folder())`).
- **Paths shown to the photographer use the platform's separators**: the path relative to the
  strip's folder in the tooltip is `make_preferred()`; the caption is the bare file name.

## Measured

Generated 50 000-shot trees (200 folders x 250 shots), warm local disk, real inotify, no
`QListView` attached unless noted; the benchmarks are hidden tests
(`test_FolderListingBench`, `test_FilmStripListingBench`).

| What | Before | Now |
|---|---|---|
| Opening: first rows | after the whole walk | 6.6-7 ms |
| Longest GUI stall while listing | the whole walk | 15-27 ms |
| One file added, work after the debounce | 0.7-0.9 s plus the re-watch | about 13 ms (315 ms with the 300 ms debounce) |
| Refresh of everything (F5) | 2.3 s, GUI frozen | 2.3 s, in the background, merged per folder |

The figures above are for a tree of 200 folders of 250 shots. The review of the background
listing found two worse shapes: one folder of 50 000 shots stalled the GUI 130-185 ms (one large
insert) and 20 000 folders of two shots 200-300 ms (many inserts). Applying streamed results under
a time budget of about 12 ms per turn, inserts of at most 2 000 rows and one insert for runs of
adjacent folders (5c0ae1f) brought the model to 13-25 ms on both. With a shown `FilmStrip` the
flat folder stalls 23-37 ms and the 20 000 tiny folders 49-143 ms; the user accepts a stall of
about 0.1 s at tens of thousands of shots, so that is left (a suspect is the thumbnail worker's
queue scan, not proven). Opening another folder over a 50 000-row one blocks 25-98 ms in the
synchronous reset. A tree of near-empty folders shows its first row after the 100 ms batch
interval.

## Consequences

- Opening, toggling and changing never block the window; the cost of a change is the cost of its
  folder. Memory is the rows plus one span per folder.
- Known limits:
  - **A refresh is folded into a running walk.** F5 or a change report during a walk does not
    run a second pass; a report for a directory still waiting is folded into that read. A
    photographer who presses F5 mid-listing gets the listing under way.
  - **`QFileSystemWatcher` is tested on Linux (inotify) only.** Windows (a refused watch on a share
    without change notification) and macOS are untested; the refusal path is exercised through a
    test seam (`WatchGate`).
  - **inotify limits.** The system's limit on watches is shared with other programs. A refusal
    shrinks the cap until the next walk, and a transient refusal is not retried before then, so
    the notice tells the user to press F5. Qt logs a warning per refused watch.
  - **Beyond the cap, a change in an unwatched folder shows only after F5.**
  - **Stalls are bounded, not absent.** Up to about 0.1 s at tens of thousands of shots with the
    real strip, which the user accepts (see Measured).
  - **Not measured:** SMB, NFS, Windows and macOS.
  - **Exit can still wait on a hung share.** Only the lister's thread is orphaned. The marks and
    EXIF loader, the thumbnail worker and the export queue are joined as before; one inside a
    read on a share that hangs holds up exit until the read returns. The user chose not to
    extend the bounded wait to them: an export may be writing its file. An orphaned lister whose
    call returns only while the process exits may run as static destructors do; that window is
    not guarded.
  - **A hung lister blocks the walks after it.** The plan's "orphan a lister stuck more than 5 s
    and start a fresh one" is not built: after the lister hangs, a new folder shows "listing" for
    ever, and F5 and the toggle are folded into the walk that never ends.
  - The 13 ms excludes the 300 ms debounce; a refresh over a slow share costs what its folders
    cost to read.
- Reviews: `docs/reviews/claude-opus-5-5_2026-10-20_background-listing.md` and
  `..._per-directory-refresh.md` found the batching stall, the duplicate and misordered rows when
  a folder is re-read mid-listing, F5's platform bindings and the refused-watch cap; the fixes are
  in 5c0ae1f and c75ae8d. `..._failure-handling-and-toggle.md` found the dropped subfolders under
  an unmounted or renamed root, the silent toggle walk, the lost first shot, the recovery that
  did not walk below, the uncancelled work of a switch off and the quadratic end-of-walk sweep.
