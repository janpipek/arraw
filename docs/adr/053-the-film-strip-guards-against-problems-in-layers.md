# The film strip guards against problems in layers, and this is the list

Status: accepted, 2026-10-20.

The user asked (2026-10-20) to "describe all measures taken to provide against problems with the
strip". ADR 052 tells how the strip lists and refreshes; this ADR is the other index: by kind of
problem, what is done about it, where, and which test fails if it stops being done. It adds no
decision. Where the code and an older document differ, the code is described. Not built is marked
"not done"; the limits are collected at the end.

Paths: `FolderLister` (`src/app/FolderLister.{h,cpp}`) is the thread that reads and watches;
`FolderWatcher` (`src/app/FolderWatcher.{h,cpp}`) owns it for the GUI thread; `ShotModel`
(`src/app/ShotModel.{h,cpp}`) holds the rows; `FilmStrip` (`src/app/ui/FilmStrip.cpp`) and
`MainWindow` (`src/app/ui/MainWindow.cpp`) are the view; the core's listing is `src/core/Shot.cpp`.
Test names are those of the `TEST_CASE`s, shortened; the files are in `tests/`.

## 1. Inputs and the filesystem

- **Hidden, junk and linked folders.** A walk must not enter `.git`, `@eaDir`, `#recycle`,
  `$RECYCLE.BIN`, `System Volume Information`, or a symbolic link to a folder. `isListedFolder` and
  `listFolder` (`Shot.cpp`) judge by `symlink_status`, so a link is never a folder; hidden files
  (leading dot) are skipped, a link to a regular file is listed. One predicate serves the listing,
  the CLI and the watcher (review 2026-10-19 B1: the watch walk had entered `@eaDir`; fixed in
  be56025). Tests: `test_Shot` "Listing recursively skips hidden and junk folders and hidden
  files", "...does not follow symbolic links to folders", "The listed-folder rule refuses hidden,
  junk and linked folders"; `test_ShotModel` "The watcher watches the folders the listing enters
  and no others".
- **Symlink loops.** Not followed because links to folders are never entered, so a loop cannot
  form. A bind-mount loop is not detected (a limit; review 2026-10-19 N3).
- **Unreadable, vanished or failing folders.** `listFolder` throws `filesystem_error` carrying the
  folder and the code; `classifyFolderError` gives `Gone` (ENOENT, ENOTDIR), `NoPermission`
  (EACCES, EPERM) or `Other` (EIO, ESTALE, ENOTCONN, timeouts). `FolderLister::handleFailure`
  acts on the kind: gone is dropped with its subtree; no permission is dropped too and counted as
  unreadable until the parent finds it readable (permission flips both ways); other keeps the rows
  as stale, counts them, and reads again at the next report or F5. A failing branch never ends
  the walk (`listShots` skips it; be56025). A mid-increment failure of the directory read is an
  error too, not a short listing. Tests: `test_Shot` "A folder that cannot be read reports why",
  "Listing recursively skips a folder that cannot be read and goes on"; `test_FolderLister` "A
  folder that vanishes during the walk is skipped", "A subfolder that loses its permission is
  removed, and returns with it", "A subfolder with an I/O error keeps its rows as stale and is
  read again".
- **A root that cannot be read.** It keeps all its rows; nothing below it is read or swept; the
  notice says "Folder unavailable"; the next report or F5 merges it normally. `ShotModel::setFolder`
  only opens the root, so a folder that cannot be opened is refused at once with the dialog and the
  model untouched. Tests: `test_FolderLister` "A root that cannot be read is unavailable, keeps its
  rows, and recovers"; `test_ShotModel` "A folder that cannot be read is refused and leaves the
  model as it was".
- **Unmount, NAS going away, rename of the root or above.** All watches report, or the subfolders
  report under their old paths, each failing as gone. Before a subfolder is dropped the lister
  opens the root (`rootOpens`); if it does not open, the subfolder is kept as stale like the root's
  rows. A directory that reads again after failing has its subtree walked and re-watched with its
  sidecars checked (`recovered`), and a subfolder reading while the root is unavailable makes the
  root be read too. The remount itself is not reported by anything: F5. Tests: `test_FolderLister`
  "A root that vanishes keeps its rows, without a watch tried on every waiting directory", "Under
  a root that is unmounted the subfolders keep their rows, and all come back", "Under a root
  renamed away a subfolder that reports keeps its rows, and brings it back" (review 2026-10-20
  failure B1; a80f6d6). Real CIFS/NFS unmount was not tried.
- **Names and separators.** Paths are handled as `std::filesystem::path`; Qt strings are made
  with `u16string` (`toQString`), so non-ASCII names survive. The tooltip's path relative to the
  strip's folder is `make_preferred()` (`ShotModel::shownName`), the caption is the bare file name;
  a trailing separator on a folder is dropped (`withoutTrailingSeparator`). Tests: `test_ShotModel`
  "A recursive model lists the subfolders and names them in the tooltip", "The display role is the
  bare file name, also in a subfolder" (the Windows failures of review 2026-10-19 B4 were the
  missing `make_preferred`).
- **Same name in two folders.** Rows are keyed by the full primary path (`index_`), shots never
  pair across folders, and folders are compared component by component in natural order with the
  exact bytes as the last tie-break, so the order does not depend on directory iteration. Tests:
  `test_Shot` "Files of different folders never pair", "Natural order ignores case and is the
  same whatever order...", "Folder order is natural, component by component", "Listing recursively
  keeps the subtrees of names equal in natural order apart". The caption is the same for both
  (a limit; tooltip tells them apart).
- **RAW+JPEG pairing, per folder only.** `groupShots` pairs by parent folder and case-insensitive
  stem; two RAWs of one stem leave every file of the stem alone (ADR 029). Tests: `test_Shot` "A
  RAW and its JPEG are one shot", "Stems pair without regard to case", "Two RAWs sharing a stem
  leave every file of the stem standing alone".
- **Companion changes.** A JPEG that appears next to a RAW, or goes, changes the shot: the diff
  makes it an update in place (the row, its marks, EXIF and thumbnail stay), not a remove and an
  insert; a change of primary is a remove and an insert (`FolderDiff.h`). Tests: `test_FolderDiff`
  "A shot whose companions changed is an update", "A change of primary is a remove and an insert";
  `test_ShotModel` "A companion that joins is an update in place", "Rows keep their marks, EXIF
  and thumbnail through a merge".

## 2. Scale

- **No cap on shots or depth.** By decision (ADR 052); nothing in the walk, the model or the diff
  has a limit but memory (rows plus one span per folder).
- **Never on the GUI thread.** The walk is `FolderLister::step`, one directory per turn of the
  lister thread's event loop. Opening, toggling and refreshing return at once; the root is only
  opened synchronously. Benchmarks: first rows in 6.6-7 ms, longest stall 15-27 ms (ADR 052,
  "Measured").
- **Batching.** The first batch with a shot goes at once; then every 512 shots
  (`FolderLister::batchShots`) or 100 ms (`batchInterval`); and at the end. Test: `test_FolderLister`
  "The lister walks in folder order and hands the shots over in batches", "...hands the first shots
  over at once, however deep".
- **Time-budgeted, queued apply.** Batches wait in `ShotModel::incoming_` and are applied by a
  zero-interval timer, about 12 ms per turn (`applyBudget`), behind the input and painting; a
  backlog cannot become one stall. Review 2026-10-20 background-listing B2 measured 130-300 ms
  stalls on one 50 000-shot folder and on 20 000 tiny ones; fixed in 5c0ae1f (now 13-25 ms for
  the model alone). Test: hidden `test_FilmStripListingBench`, `test_FolderListingBench`.
- **Insert chunking.** An insert has at most 2000 rows (`maxInsertRows`) and one insert covers a
  run of adjacent new folders (`ShotModel::insertRun`), so neither one huge nor 20 000 tiny
  `rowsInserted`. Test: `test_FolderLister` "A model takes its rows from the walk it asked for".
- **Linear per-folder merge.** Rows are one span per folder; a refresh diffs a folder's span
  against its fresh list in time linear in the folder (`diffFolder`), one signal per run, and past
  64 runs (`maxFolderRuns`) or rows that stay in another order it resets that folder instead
  (`resetFolder`). Tests: `test_FolderDiff` "More runs than allowed ask for a reset", "Rows that
  stay in another order...", "Fifty thousand rows are merged in a moment, even in the worst shape";
  `test_ShotModel` "A refresh signals one insert or remove per run of rows", "A scattered change
  resets the model and keeps what the rows know".
- **End-of-walk sweep is linear.** Gone folders are found in one pass over the spans against the
  sorted outermost stale folders, and removed one run at a time from the last (review failure R5;
  a80f6d6). Test: `test_FolderLister` "The end of a walk keeps the rows below thousands of stale
  folders in one pass".
- **Sidecar rechecks only for what changed.** A merge queues `Recheck` jobs (cheap: the stamp is
  compared first) only for a refresh of everything, a folder the watcher named (`recheck`), or a
  folder whose diff changed. Test: `test_ShotModel` "A merged folder's sidecars are checked when it
  reported a change, and only then"; `test_FolderLister` "A change rechecks the sidecars of its
  folder only".
- **The 100k note.** At 100 000 rows (`largeFolderShots_`) a softly shown label says so, once per
  listing; it blocks nothing and is plain text. Test: `test_FilmStripNotice` "A listing past the
  large-folder size says so once, softly".
- **Benchmarks.** `[.bench]` cases, hidden, run only when a tag names them
  (`test_FolderListingBench`, `test_FilmStripListingBench`, trees from `tests/support/BenchTrees.h`,
  stall measured by `StallProbe.h`). The review found the first bench did not measure its claim
  (background R6); fixed in 5c0ae1f.

## 3. Concurrency

- **One thread owns the watcher.** `QFileSystemWatcher`, the debounce timer, the tree, the dirty
  set and the budget live on the lister thread and are touched there only; other threads use only
  `start`, `cancel`, `setMaxWatchedFolders`, `setWatchGate`, `setReadHook`, `watchedDirectories`
  and `simulateDirectoryChanged`, each atomic or mutex-guarded (header: "Safe from any thread").
  `shutDown` runs on the lister thread through a direct connection to `QThread::finished`.
- **Generation numbers.** Every walk has a number (`ShotModel::walk_`, never 0), stamped on all it
  delivers and on every change read after it; the loader has its own generation for the marks and
  EXIF jobs. A toggle starts a new walk but not a new loader generation, so reads owed to the rows
  that stay still land. Test: `test_FolderLister` "A model takes its rows from the walk it asked
  for, and drops other walks"; `test_ShotModel` "Changing the folder drops what the loader was
  doing for the old one".
- **Stale batches are dropped** at three places: at the lister (`current()` before each directory,
  after the watch, after the hook, after the read), in `FolderWatcher::takeStatus`/`finishWalk`,
  and in `ShotModel::applyQueued` (`walk != walk_`). A batch of the last walk is still taken after
  its end (a directory re-read after a change). Test: `test_FolderLister` "A toggle drops the
  batches of the walk before it, queued or late".
- **Cancellation.** `start` writes the wanted walk atomically before queueing, so the walk under
  way stops before its next directory; `cancel` writes 0. A walk cancelled while a watch or the
  hook is blocked does not go on to read (review failure R3). Tests: "A new folder cancels the
  walk under way", "A toggle during a walk abandons it before its next directory", "A walk
  cancelled while a watch is asked for does not go on to read the directory", "A new folder
  cancels the change being read for the old one".
- **Queued apply keeps order.** Results and the walk's end go through one FIFO; the end is applied
  after the batches before it, and the loads of the last rows count before it may report the
  loading finished (`applyQueued`, `handOver`). Batches that arrive when the folder changed from a
  view handler are not handed to the loader (`generation_` compared).
- **Orphaning a hung lister.** The destructor of `FolderWatcher` cuts the link, cancels, quits and
  waits at most `defaultExitWait` (2 s); then the thread is released, deletes itself when it
  finishes, and the lister it owns delivers nowhere. `Link` (a mutex around the target pointer)
  posts under the lock, so the watcher cannot die between the check and the post; the lister holds
  the link, never the watcher or the model. Test: `test_FolderLister` "A watcher whose lister hangs
  in a read is let go at exit, within the wait"; "The watcher goes while changes are pending or
  being read".
- **Shutdown order.** `ShotModel::loader_` is destroyed first in the destructor (joins its thread
  before the callback's members go; the `jthread` is declared last); the lister's watches are
  released on its own thread.
- **Debug span assertion.** `checkSpans` (Debug builds only) asserts after every apply that the
  spans tile the rows in order. It is exercised by every model test run in Debug, with no test of
  its own.
- **Thread affinity of results.** Receivers run on the lister thread and only marshal
  (`Link::post`, `Qt::QueuedConnection`); the model is touched on the GUI thread only; EXIF is
  read by the loader, not by a tooltip request (a tooltip is drawn on the GUI thread).

## 4. Watching

- **Watch, then read.** Each directory gets its watch before its read; a change after the read is
  reported, one before it is in the read. A directory that got its watch only after its read (it
  could not at first, was promoted, or Qt dropped it) is read once more (`step`, `applyMoves`).
  Tests: `test_FolderLister` "A directory is watched before it is read, and changes during the walk
  are read", "A directory that gains a watch is read once more", "A directory whose watch failed
  because it did not open is watched once it reads".
- **Dropped watches.** Qt drops the watch of a directory removed, moved or unmounted; `readChanged`
  compares with `watcher_->directories()` and `forgetDroppedWatches` at the start of a walk and on
  recovery, so the next read watches it again. A report that arrives before a walk begins is kept
  (review refresh R2; c75ae8d). Tests: "A watch Qt drops just before a walk begins is had again by
  that walk"; the `simulateDirectoryChanged(..., watchDropped)` seam.
- **Budget.** At most 1000 watches (`filmStrip/maxWatchedFolders`, hidden, clamped to
  1..1 000 000), shallowest first then `folderPrecedes` order (`WatchBudget`); a shallower folder
  that appears evicts the deepest watched; a place freed goes to the next waiting folder, which is
  read once more. Tests: `test_WatchBudget` (all ten cases); `test_FolderLister` "With a cap the
  shallowest directories are watched, and the rest are reported", "A shallower directory that
  appears takes the place of a deeper one".
- **Refused watch.** An `addPath` refused for a directory that opens is the system's limit: the
  cap shrinks to what was accepted until the next walk. A directory refused for good is set aside
  (`refused`) and does not take freed places (review refresh R3; c75ae8d); one that does not open
  keeps its place until its read settles it, and no watch is tried on every waiting directory under
  a vanished root (R5). Tests: "A watch the system refuses shrinks the cap, and the next walk tries
  again"; `test_WatchBudget` "A refused watch shrinks the budget to what the system gave", "A
  refused directory does not take the places freed after it".
- **Notice and F5.** A title-bar button says "N folders not watched" (and "could not be read",
  "Folder unavailable") with a tooltip; click or F5, plus the platform's own Refresh bindings, or
  View > Refresh Folder, re-reads in the background, shows "refreshing..." and coalesces repeats.
  Unwatched folders are not polled. Tests: `test_FilmStripNotice` "The notice counts the folders
  that are not watched, and a click refreshes", "Refresh Folder is in the strip's context menu",
  "Refresh Folder says that it runs, and a press during a walk is taken in" (R1, R6 of the refresh
  review).
- **Latency and debounce.** 300 ms of quiet, but no later than 2 s after the first report
  (`maxLatency`), so a long import shows about every 2 s; reports for a directory waiting to be
  read are folded into that read; a changed directory is read alone. Test: `test_FolderLister`
  "A change in a subfolder reads that folder again, and no other", "A new nested subtree is walked,
  and one moved away goes with one remove".

## 5. Consistency of the view

- **Selection and active shot across merges.** A merge that is one reset stashes the selection,
  current and anchor by path and restores them (`stashSelection`/`restoreSelection`, not for a new
  folder). The subfolder toggle resets nothing. Tests: `test_FilmStripSubfolders` "The film strip
  keeps its selection when a refresh is too scattered to merge by runs", "...toggle resets
  nothing, keeps the selection".
- **Pending activation.** A photo opened before the walk reaches it is kept as `pendingActive_`
  and becomes active when its row arrives (`takeInsertedRows`; the companion counts); a listing
  that ends without it leaves the photo open and no shot active. `setActive` on an unlisted shot
  during a listing first clears the active shot, so marks cannot go to the previous one (background
  B1; 5c0ae1f). Tests: `test_FilmStripListing` "A shot to activate waits for its folder to be
  listed", "A photograph opened before the listing brings it leaves no other shot to rate", "A
  photograph restored from below the strip's folder waits for its listing".
- **First-shot activation.** The strip announces the first shot of a listing once, queued out of
  the model's insert (no dialog inside a batch); the window opens it only if nothing else became
  active and the open photograph was not edited since (`armFirstListed`, `editedSinceAsked`); a
  listing with no shot opens nothing, now or later. `activateFirstListed_` is reset on a file
  chosen meanwhile. A toggle before the first shot still announces it (failure B3; a80f6d6).
  Tests: `test_FilmStripListing` "The strip announces the first shot it shows of a listing, once",
  "The window opens a folder's first shot once the listing brings it", "Leaving the open photograph
  for a slow folder is asked about once", "A folder whose listing brings no shot opens nothing at a
  later listing"; `test_FilmStripSubfolders` "A toggle before a folder's first shot arrives still
  announces that shot".
- **Save/Discard/Cancel when leaving a photo.** Every route that replaces the open photo asks
  `confirmLeavingPhoto` once, after the new file is known to open (`openFile`, `activateShot`,
  `openFolder`); Cancel leaves strip, selection and photo as they were.
- **The toggle guard.** Switching subfolders off with an open photo below the strip's folder (or
  pending) asks first (`subfoldersLeaveGuard_`); Cancel puts the button back. Then the strip clears
  the active shot and the window opens the first shot left. Test: `test_FilmStripSubfolders`
  "Switching subfolders off with a subfolder photograph open asks about its edits".
- **The remembered folder is the strip's folder** (`rememberFolder(filmStrip_->folder())`), not the
  directory of the opened file; the `includeSubfolders` setting restored at start is applied only
  when the remembered file lies below the folder. Test: `test_FilmStripSubfolders` "Only the strip's
  folder is remembered, not the folder of a photograph below it".
- **openFile asks the model.** Whether the strip lists a file is `shotContaining`, not a string
  prefix test, so a path under a skipped (linked or hidden) folder switches the strip to its
  folder; `listingReaches` stats the components for a listing under way and uses the same
  `isListedFolder`. Test: "A photograph the strip does not list switches the strip to its folder".

## 6. Sidecars

- **The badge** is `marksLoaded && sidecarExists`, the flag from the stamp (0 means no file) taken
  on the loader thread before the read, so a write in between shows up as a change; it follows the
  loader's `Marks`/`Recheck` results and `noteSidecarWritten`. Test: `test_ShotModel` "The sidecar
  role says whether a sidecar file exists". A companion's own sidecar never shows the badge (the
  badge is the primary's; review 2026-10-19 N11); not documented on the role.
- **Own write or external change.** The application notes the stamp it wrote
  (`FolderWatcher::noteOwnWrite`, path normalised); a recheck that finds the same stamp raises
  nothing, a different one raises `sidecarChangedExternally`, and the window reloads the open photo
  if it has no unsaved edits, keeps the edits and says so if it has, and keeps the history when
  nothing differs. Tests: `test_ShotModel` "The watcher reports a sidecar another program changed,
  and not the application's own"; `test_FolderLister` "A change rechecks the sidecars of its
  folder only, and own writes stay quiet". Hand marks set after a job was queued win over its
  reading (`marksSet`; `test_ShotModel` "setMarks updates the row, and a reading under way cannot
  undo it").
- **Format version (ADR 051).** A newer arraw reads every older version; an older one reads what
  it understands with `Notice::NewerSettingsVersion`, and never writes over a newer sidecar, marks
  included (`writeSidecar` throws, the file stays). For the strip this means a rating on a shot
  whose sidecar is newer fails with an error rather than damaging it. Tests: `test_Sidecar` "A
  sidecar of a newer version is read but never written over", "...refused for marks too".

## 7. Presentation

- **Empty states.** "Listing folder...", "No photographs in this folder", "No shots match the
  filter" (`updateEmptyHint`, an overlay transparent to the mouse). Test: `test_FilmStripListing`
  "The strip says that it is listing, and what an empty strip means".
- **Busy notes.** The folder label adds "listing... N shots" or "refreshing... N shots" after a
  delay (`busyNoteMs`), updated by a timer, so a quick walk never flickers.
- **Notices.** One button joins "Folder unavailable", "N folders could not be read" (five paths
  listed, "and N more") and "N folders not watched"; a notice that has nothing to act on (the large
  folder) is a plain label, not a button. Plurals use `%n`. Tests: `test_FilmStripNotice` "The
  notice says that the folder is unavailable while its rows stay, and clears", "The notice counts
  the folders that could not be read, with the unwatched ones", "The notice stays hidden when every
  folder is watched, and clears with a new folder".
- **Tooltip escaping.** The notice's tooltip is built with `Qt::convertFromPlainText`, so a folder
  called `<b>x</b>` shows as written. Test: "The notice's tooltip shows a folder's path as it is,
  never as markup". Not done: the shot's tooltip (`shotTooltip`) is not escaped; a file name that
  looks like markup may render as rich text (not verified, not tested).
- **Caption elision.** The caption is elided in the middle to the cell's width; hidden below a
  cell content height of 64 px (`marksMinHeight`), as are the stars and the badge. Cells have a
  minimum content of 32 px and follow the strip's height (`cellSide`). Test: `test_FilmStripRules`
  (cell side), `test_FilmStripSubfolders` "The film strip's cells are taller than wide, by a caption
  line, and fit the strip".

## 8. Tests and process

- **Deterministic waits.** Tests wait for a condition (`waitUntil`), not for a time; the read of a
  directory can be held with `ListingGate` (`tests/support/ListingGate.h`, through the read hook)
  and the model driven without a thread with `ListingFeed.h`; changes are fed with
  `simulateDirectoryChanged`, not by inotify's timing; the old recursive listing is kept as
  `ReferenceListing.h` to compare against. Review findings on timing (2026-10-19 R8, background R7) and on a fixture used after free
  (failure R6) were not re-checked for this ADR.
- **Seams in production code.** `FolderLister::setReadHook`, `setWatchGate`,
  `simulateDirectoryChanged` (and their `FolderWatcher` forwards), `FolderWatcher::setExitWait`,
  `FilmStrip::setReadHook`/`setLargeFolderShots`. They are named in the reviews as a cost
  (background N1, refresh N5, failure N7) and kept.
- **Reviews of this work**, all in `docs/reviews/`: `claude-opus-5-5_2026-10-19_film-strip-
  subfolders-badge.md` (B1-B4, R1-R8; fixes be56025, then the plan), `..._2026-10-20_background-
  listing.md` (B1, B2, R1-R7; fixes 5c0ae1f), `..._2026-10-20_per-directory-refresh.md` (B1,
  R1-R6; fixes c75ae8d), `..._2026-10-20_failure-handling-and-toggle.md` (B1-B3, R1-R6; fixes
  a80f6d6). The widget tests passed in each review; the known flakes (brush repaint, mask overlay)
  are outside the strip and listed in `docs/todo.md`.

## Problem -> measure -> test

| Problem | Measure | Test |
|---|---|---|
| Walk enters `@eaDir`, `.git`, links | One `isListedFolder` for listing and watcher | `test_Shot` junk/link cases; `test_ShotModel` "watches the folders the listing enters" |
| One bad folder ends the walk | Skip the branch, classify the error | `test_Shot` "skips a folder that cannot be read"; `test_FolderLister` "A folder that vanishes during the walk" |
| Permission lost or regained | Dropped, counted, back with the parent's read | "A subfolder that loses its permission..." |
| I/O error on a share | Keep rows as stale, read again | "A subfolder with an I/O error..." |
| Root unreadable | Keep rows, "Folder unavailable", merge on return | "A root that cannot be read is unavailable..." |
| Unmount or rename of the root | Probe the root before dropping a subfolder; re-walk on return | "Under a root that is unmounted...", "...renamed away..." |
| Same name, different folder; order | Full-path keys, component-wise natural order | `test_Shot` order and pairing cases |
| Freeze on opening | Background walk, batches, 12 ms apply, 2000-row inserts | `test_FolderLister` batching cases; hidden benches |
| Quadratic diff, whole-tree refresh | Span per folder, linear diff, reset past 64 runs | `test_FolderDiff`; `test_ShotModel` merge cases |
| Slow end-of-walk sweep | One pass over spans | "The end of a walk keeps the rows below thousands of stale folders in one pass" |
| Old walk delivers late | Walk numbers, atomic wanted walk, three drop points | "drops other walks", "A toggle drops the batches..." |
| Hung share holds up exit | 2 s wait, orphaned lister, guarded link | "A watcher whose lister hangs in a read is let go..." |
| Change missed between read and watch | Watch, then read; re-read after a late watch | "A directory is watched before it is read..." |
| Qt drops a watch | Compare with the watcher's list, watch again | "A watch Qt drops just before a walk begins..." |
| inotify limit | 1000-watch budget, shallowest first, shrink on refusal | `test_WatchBudget`; "A watch the system refuses shrinks the cap..." |
| Unwatched folders unseen | Notice, F5, Refresh Folder | `test_FilmStripNotice` |
| Selection lost in a reset | Stash and restore by path | `test_FilmStripSubfolders` "keeps its selection when a refresh is too scattered" |
| Photo opened before its row exists | Pending activation, clear previous active | `test_FilmStripListing` pending cases |
| Unsaved edits lost on leaving | `confirmLeavingPhoto` on every route, toggle guard | "Switching subfolders off with a subfolder photograph open..." |
| Wrong folder remembered | Remember the strip's folder | "Only the strip's folder is remembered..." |
| Own sidecar write reported as external | Stamp noted at write | "The watcher reports a sidecar another program changed..." |
| Newer sidecar damaged | Never written over | `test_Sidecar` newer-version cases |
| Path shown as markup | Plain-text conversion for the notice | "The notice's tooltip shows a folder's path as it is..." |
| Timing-dependent tests | `waitUntil`, gates, simulated reports | the support headers above |

## Known gaps and limits

Copied from ADR 052 and the reviews, and checked against the code:

- **Not done:** orphaning a lister stuck more than 5 s and starting a fresh one. After a hung
  lister a new folder shows "listing" for ever; F5 and the toggle are folded into the walk that
  never ends (ADR 052; failure R2).
- **A refresh is folded into a running walk**: F5 mid-listing gives the listing under way.
- **Exit can still wait on a hung share** in the marks and EXIF loader, the thumbnail worker or
  the export queue (only the lister is orphaned; the user chose so). An orphaned lister whose call
  returns only while the process exits is not guarded against static destruction.
- **inotify only.** `QFileSystemWatcher` is tested on Linux; Windows (watch refused on a share,
  about 1000-handle limit, review 2026-10-19 R6), macOS, SMB and NFS are untested and unmeasured.
  A real unmount was only simulated.
- **A transient refusal of a watch is not retried** before the next walk; the notice says F5. Qt
  logs a warning per refusal.
- **Beyond the cap, a change in an unwatched folder shows after F5.** Nothing polls.
- **Bind-mount loops are not detected** (only symlinks are refused).
- **Stalls are bounded, not absent**: up to about 0.1 s at tens of thousands of shots with the
  real strip; opening another folder over 50 000 rows blocks 25-98 ms in the synchronous reset; a
  tree of near-empty folders shows its first row after the 100 ms batch interval.
- **Same-named shots in two folders have the same caption**; only the tooltip distinguishes them.
- **A companion's sidecar never gives the badge.**
- **The shot tooltip is not escaped** (see 7); not verified.
- **Test seams are production API**, and `checkSpans` runs in Debug only.
- **Not verified when this ADR was written:** a Release build's timings; behaviour after the
  orphaned thread returns following `QCoreApplication`'s destruction; the current state of the
  files under edit by another agent (`FilmStrip.cpp`, `MainWindow.cpp`), read as of 2026-10-20.

## Reviews

- `docs/reviews/claude-opus-5-5_2026-10-19_film-strip-subfolders-badge.md`
- `docs/reviews/claude-opus-5-5_2026-10-20_background-listing.md`
- `docs/reviews/claude-opus-5-5_2026-10-20_per-directory-refresh.md`
- `docs/reviews/claude-opus-5-5_2026-10-20_failure-handling-and-toggle.md`
