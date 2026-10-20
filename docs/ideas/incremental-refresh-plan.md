# Film strip: background listing and per-folder refresh — plan

Status: proposed, 2026-10-20. Architecture only; nothing is built yet.

## Goal

A photographer opens a large tree (50 000 shots, local or on a NAS) with "Include subfolders" on.
The window stays responsive the whole time. The strip fills in while the tree is read, and a change
on disk updates only the folder that changed. Today the window freezes for the whole walk:
- `ShotModel::setFolder` lists the tree synchronously.
- Every debounced watcher event runs `ShotModel::refresh`, which re-lists the whole tree, diffs it
  quadratically (review R2) and rechecks every loaded sidecar.
- `FolderWatcher::rewatch` then walks the tree a second time.

Measured cost: about 0.7-0.9 s per refresh at 50 000 shots on a warm local disk, and tens of seconds
on SMB/NFS
([review 2026-10-19](../reviews/claude-opus-5-5_2026-10-19_film-strip-subfolders-badge.md),
R1, R2, R3, R7, B1).

## Decisions (user, 2026-10-20)

| Question | Answer |
|---|---|
| Cap on shots | **None.** |
| Cap on depth | **None.** |
| Cost | **Incremental and asynchronous:** a background listing, and refreshes per folder. |
| Cap on watches | **Kept** at 1000 directories (`FolderWatcher::maxWatchedFolders`), with a **visible notice** that N folders are not watched, and a **manual refresh key**. |
| Very large trees | A soft warning past about 100 000 is acceptable. It does not block anything. |

Answers to the open questions (user, 2026-10-20): all nine as recommended. In short: (1) a
vanished root keeps its rows with a "Folder unavailable" notice; (2) an unreadable subfolder is
removed; (3) the strip updates about every 2 s during a long import; (4) opening a folder keeps its
synchronous root check and error dialog; (5) the 100k warning counts shots, shown in the strip
notice; (6) a hung lister thread is orphaned at exit; (7) the 1000-watch cap gets a hidden
`filmStrip/maxWatchedFolders` override; (8) Refresh Folder in the View menu and in the strip's
context menu; (9) polling of unwatched folders stays out of scope.

## Assumed baseline

The working tree already holds uncommitted fixes for B1, R3, R4 and B2/B3, and another agent is still
editing it:
- `isListedFolder` is the predicate shared by the listing and the watch walk.
- `filesUnder` uses an explicit stack, so a folder that fails is skipped and its siblings are still
  listed.
- `compareFolders` compares component by component.
- `restoreOpenPath` knows about subfolders, and `openFile` asks `shotContaining`.

This plan assumes those changes land first (step 0). It does not repeat them.

## Shape in one paragraph

The **core** gets a non-recursive primitive, `listFolder(dir)`. It returns that one directory's shots
*and* its listable subfolders from one `readdir`, plus a public folder-order comparator.

A **`FolderLister`** is a `QObject` on its own `QThread`, owned by `FolderWatcher`. It owns:
- the directory tree;
- the `QFileSystemWatcher`;
- the watch budget;
- the per-directory debounce;
- a queue of directories to read.

It walks one directory per event-loop turn, and watches each directory **before** it reads it. It
streams `FolderResult` batches to the GUI thread.

**`ShotModel`** becomes passive about the filesystem. It keeps its rows as contiguous per-folder
spans in folder-major order. It applies each `FolderResult` as "replace this folder's span", with a
linear diff that emits one insert or remove per run.

**`FilmStrip`** shows:
- a busy note while a walk runs;
- a notice for unwatched or unreadable folders;
- a pending activation, so that opening a photo still selects it when its folder arrives later.

F5 (`QKeySequence::Refresh`) re-walks the tree in the background by the same path.

## 1. Core: one directory at a time

```cpp
// include/Shot.h
/// @brief One directory, read once.
struct FolderListing {
    /// Shots of the directory itself, in ::arraw::groupShots order.
    std::vector<Shot> shots;
    /// Sub-directories ::arraw::isListedFolder accepts, in ::arraw::folderPrecedes order.
    std::vector<std::filesystem::path> folders;
};

/// @brief Reads one directory: its shots and the subfolders a recursive listing enters.
/// @throws std::filesystem::filesystem_error carrying the error code (ENOENT, EACCES, EIO, ...).
[[nodiscard]] FolderListing listFolder(const std::filesystem::path& folder);

/// @brief Orders folders component by component in natural order, so a subtree follows its root.
[[nodiscard]] bool folderPrecedes(const std::filesystem::path& a, const std::filesystem::path& b);
```

- `listShots(folder, recursive)` keeps its contract for the CLI and Python. It is re-implemented as
  a **pre-order DFS over `listFolder`**, where each directory's children are visited in
  `folderPrecedes` order.
  - Pre-order with sorted children gives exactly the component-wise order that `listShots` sorts
    into now. The final `std::map` and sort go away.
  - It is the same order the lister streams in, so the streamed rows are always appended at the end
    (see 2).
- `filesystem_error` derives from `std::runtime_error`, so today's `catch (const std::runtime_error&)`
  still works. The error code lets the app tell "gone" (ENOENT, ENOTDIR) from "unreadable" (EACCES)
  and from "transient" (EIO, ENOTCONN, ESTALE, ETIMEDOUT, anything else).
- Cost: use `directory_entry::is_regular_file(ec)` and the cached `symlink_status` instead of
  `fs::is_regular_file(path)`. The latter stats every file, which is the dominant per-entry cost on
  SMB (review N1). Measure this with the benchmark, because libstdc++ caches `d_type` but MSVC
  behaves differently.
- An open question for core: should `FolderListing` also carry the directory's modification stamp?
  It is useful for the "verify after watch" fallback (option 4b). It is not needed in the
  recommended design.

## 2. ShotModel: rows as folder spans, merged per folder

### Data

```cpp
struct Span { int first = 0; int count = 0; };           // rows of one folder, contiguous
std::vector<Row> rows_;                                    // unchanged: random access for views
std::map<fs::path, Span, FolderOrder> spans_;              // only folders that have rows
std::unordered_map<fs::path, int> index_;                  // primary -> row, as now
std::uint64_t generation_;                                 // folder generation: loader and lister
bool listing_ = false;                                     // a walk is under way for generation_
```

`FolderOrder` wraps `arraw::folderPrecedes`. **Ordering invariant:** rows are folder-major in
`FolderOrder`, and within a folder in `groupShots` order. Every row of a folder lies in its span. A
subtree is a contiguous run of spans, so a contiguous run of rows.

### Applying a folder

`applyFolder(const FolderResult&)`:
1. **New folder** (no span). Find the insertion row as `spans_.lower_bound(folder)->first`, or
   `rows_.size()` when it is last. Make one `beginInsertRows` for the whole folder, one
   `vector::insert` of the range, then shift `first` of the later spans (O(F)) and `index_` of the
   later rows (O(N − pos)). During the initial walk pos == N, so this is O(batch).
2. **Known folder.** Use a linear diff against its span, `diffFolder(oldPrimaries, newShots) ->
   std::vector<RowRun>`, a pure function in `src/app/FolderDiff.h`:
   - Put the new primaries in an `unordered_set`. Old rows missing from it become *remove runs*,
     applied from the back with one `beginRemoveRows` per run.
   - What is left is a subsequence of the new list, in the same order, because `groupShots` sorts by
     a total order on the primary. Walk both lists with two pointers to get *insert runs* and
     *update* rows (companions changed: `dataChanged` on `CompanionsRole`, `FormatLabelRole` and
     `ToolTipRole`).
   - Cost: O(n_folder) for the diff, plus O(N) per run for the vector move, `index_` and the
     proxy's own mapping update. A `QSortFilterProxyModel` is O(N) per source insert anyway, so a
     run, not a row, is the unit to minimise.
   - **Fallback:** above 64 runs in one apply (an interleaved copy of thousands of files), use
     `beginResetModel`/`endResetModel` instead. The strip restores its selection, current index and
     scroll anchor around the reset (see 5). Rows keep their marks, EXIF and thumbnails across it,
     because the model keeps `Row` data.
3. **Gone or unreadable** (see 6): `removeSubtree(folder)` removes the contiguous row range of
   `[folder, first span not under folder)` with one `beginRemoveRows`, or leaves the span as it is.
4. **Rechecks only for the folders applied.** Queue `JobKind::Recheck` for the loaded rows of the
   changed folders only, not for all N rows as `refresh` does now. A rating write in a 250-shot folder
   then stats 250 sidecars, not 50 000. A manual refresh rechecks everything.
5. New primaries go to `queueLoads`, so marks come before EXIF, as now. `FilmStrip`'s existing
   `rowsInserted` hook feeds `ThumbnailWorker::addShots`.

`reindex()` (full O(N)) is replaced by a tail update from the edited row. `refresh()` is removed: the
watcher's lister drives every re-list. The quadratic `std::ranges::find(added, …)` (R2) goes with it.

### New ShotModel API

| Member | Purpose |
|---|---|
| `setFolder(folder, recursive)` | Synchronous root check (opens the root with `directory_iterator`; throws as now), then a reset to empty, `++generation_`, `listing_ = true`. **No listing.** The `modelReset` it emits is what starts the watcher's walk, as `rewatch` hangs off `modelReset` today. |
| `generation()` | For the lister to stamp its results. |
| `applyFolders(generation, std::vector<FolderResult>)` | One batch. Stale generations are dropped. |
| `removeSubtree(generation, folder)` | A sub-directory went away. |
| `finishListing(generation, ListingSummary)` | `listing_ = false`. `loadingFinished` is now emitted only when `!listing_ && pending_ == 0`. |
| `isListing()` and signal `listingChanged(bool)` | For the busy note. |
| `setRecursive(bool)` (step 6) | Toggles without a reset (see 5). |
| `shotsListed()` | A running count, for progress and the 100k warning. |

A plus of this split: **the model's tests need no threads and no filesystem.** They feed
`FolderResult`s directly.

## 3. FolderLister: walk, watch and debounce on one thread

### Threading options

| Option | Shape | For | Against |
|---|---|---|---|
| **A (recommended)** | `FolderLister` `QObject` moved to a `QThread`, owned by `FolderWatcher`. It owns the `QFileSystemWatcher`, the tree, the budget, the debounce timer and the work queue. It walks **one directory per event-loop turn** (`invokeMethod(this, &step, Qt::QueuedConnection)`). | Watch-before-list per directory closes R7 by construction. Watch syscalls are off the GUI thread too: on Windows each `addPath` opens a change handle, which can block on a share. Serial, so no two listings race. New requests and `directoryChanged` are served between directories, which gives prompt cancellation. | A second threading idiom beside the `std::jthread` loader. `QFileSystemWatcher` off the GUI thread is supported, but less travelled (see Risks). |
| B | A `std::jthread` lister like `ShotModel::Loader`, walks only. `QFileSystemWatcher` stays on the GUI thread in `FolderWatcher`. | Same pattern as the loader. Smaller change. | Watches are added after the read, so every directory needs a re-list after its watch, which doubles the `readdir`s, or a stamp check (4b). `addPath` runs on the GUI thread (Windows/NAS). |
| C | `QtConcurrent::run` per job, with `QFutureWatcher`. | Least code. | No serial order: results of two jobs on one folder can arrive in either order, so each needs a per-folder sequence number. Cancellation is coarser. |

### Lister state (all on the lister thread)

```cpp
struct Dir { int depth; bool watched; std::uint32_t seenInWalk; };
std::map<fs::path, Dir, FolderOrder> tree_;   // every listed directory, empty ones included;
                                              // a directory's subtree is the range up to its next
                                              // sibling, so no child lists are stored
WatchBudget budget_;                          // see 3c
std::deque<fs::path> work_;                   // directories to read, in display order
std::set<fs::path, FolderOrder> dirty_;       // directories with events, waiting for the debounce
std::atomic<std::uint64_t> generation_;       // written by the GUI thread, read before each step
```

### a. Initial walk (setFolder, toggle on, F5)

Signal flow:

```
GUI   FilmStrip::setFolder → ShotModel::setFolder (root check, reset, ++gen)
        └ modelReset → FolderWatcher → lister.start(root, recursive, gen)   [queued]
LST   start: clear tree_ and work_, unwatch everything; work_ = {root}
      step:  d = work_.front()
             budget_ grants d a watch? → watcher.addPath(d) BEFORE reading
             listFolder(d) → FolderResult{d, Listed, listing}
             push d's folders to the FRONT of work_, in order       (pre-order DFS)
             add to batch; flush when ≥ 512 shots, ≥ 100 ms since the last flush, or work_ empty
             if gen changed → drop batch and work_, stop
      flush: post applyFolders(gen, batch) to the model             [queued lambda, as the Loader]
      done:  post finishListing(gen, {folders, shots, unwatched, unreadable})
GUI   ShotModel::applyFolders → appends spans (always at the end, by the DFS order)
        → rowsInserted → thumbnails.addShots, FilmStrip pending activation, busy count
```

- The first flush happens right after the root, so the root's shots show at once.
- Cancellation:
  - A new `setFolder` or a toggle bumps `generation_`. The lister checks it before each directory,
    and the model drops stale batches.
  - A `readdir` already under way finishes first. A hard-mounted NFS that hangs is a known limit
    (Risks).
- Non-recursive is the same path, with `folders` ignored. The tree is `{root}`.

### b. Per-directory refresh

```
LST   directoryChanged(d) → dirty_.insert(d); debounce.start()      (300 ms trailing, as now)
      timeout: for each d in dirty_ (FolderOrder): queue d in work_ unless already queued
      step(d) as above, then compare listing.folders with d's children in tree_:
        new child c   → tree_ insert, queue c (its subtree streams in as a walk)
        missing child → post removeSubtree(gen, c); erase its range in tree_; release its watches
                        (budget_ may promote others, see 3c)
      d itself fails ENOENT → as a missing child of its parent (and the parent's event re-lists it)
```

- Pairing is per folder (ADR 029), so a re-list of `d` can change only shots in `d`. Its span diff
  (2) is complete.
- Only `d`'s rows are rechecked.
- **Optional:** a maximum latency (for example 2 s), so a long import updates while it runs instead
  of only after it pauses. This is an open question.

### c. Watch budget (cap 1000, shallowest first)

`WatchBudget` is a pure class in `src/app/WatchBudget.h`, unit-tested without Qt:

```cpp
struct Key { int depth; fs::path path; };      // ordered by depth, then FolderOrder
std::set<Key> watched_, waiting_;              // invariant: max(watched_) < min(waiting_),
                                               // |watched_| == min(cap, total)
Grant add(path, depth);   // {watch: bool, evict: optional<path>}
std::optional<fs::path> remove(path);           // a waiting directory promoted, if any
int unwatched() const;
```

- The DFS reaches deep directories before shallow siblings. When the budget is full and a shallower
  directory arrives, the deepest watched one is **evicted**: `removePath` it, and add the new one
  before its read. That is at most a few hundred cheap inotify calls on a big tree. The result
  equals "the first 1000 of a BFS", which is today's policy, but deterministic (sorted), and junk
  folders are excluded by `isListedFolder` (B1).
- A promoted directory gets its watch and then **one re-list** (rule in 4).
- If `addPath` fails (ENOSPC: the system's inotify limit is shared with other programs), the
  directory counts as unwatched. The budget shrinks to what the system gave.
- After each walk or change, the lister posts `watchStatus(gen, watched, unwatched)`.

### d. Manual refresh (F5)

`FolderWatcher::refreshAll()` → `lister.refresh(gen)` → a full walk by the same path, with a fresh
`seenInWalk` epoch:
- It is a **merge, not a reset.** Every directory's result replaces its span, so the selection, the
  marks and the thumbnails stay.
- At the end, any directory in `tree_` not seen in this epoch is removed (`removeSubtree`). This is
  a sweep.
- The model queues rechecks for **all** rows. This is the one way to pick up a sidecar rewritten in
  place, which inotify on directories does not see (`FolderWatcher.h` says so already).
- The watch set is re-evaluated as in the initial walk.

## 4. Missed-event window (R7)

Rules:
1. **Watch, then read.** The lister adds a directory's watch before `listFolder` on it. An event
   after the watch is seen. A change before it is in the read. With option A this holds for every
   directory the budget grants.
2. **A watch added to a directory already read triggers one re-list** of it, through promotion or a
   watch Qt dropped and re-added. That re-list counts as the read of rule 1.
3. **A new sub-directory** is found by the parent's event (the parent is watched before its read),
   then watched before it is read itself. Files written into it between its `mkdir` and its watch
   are in its read. No gap.
4. **Events during a walk.** For a directory not yet read, the event is dropped (it is in `work_`;
   dedupe). For a directory already read, it goes to `dirty_` and is re-listed after the debounce.
   The lister is serial, so a later result always wins, and "replace span" is idempotent.
5. **A watch lost silently.** Qt drops a watch on `IN_DELETE_SELF`, `IN_MOVE_SELF` or `IN_UNMOUNT`.
   After each event the lister checks `watcher.directories()` against `budget_.watched_`. A
   directory that still lists but lost its watch is re-watched and re-listed (rule 2).
6. **Not covered.** inotify queue overflow (`IN_Q_OVERFLOW`, which Qt does not surface), directories
   beyond the budget, and sidecars rewritten in place. All three are covered by F5, and the notice
   says so for the second.

If option B is chosen instead, rule 1 becomes either (4a) re-list every directory once after its
watch, which doubles the reads, or (4b) store the directory's mtime from before the read and re-list
only if a stat after the watch differs. 4b is cheap, but mtime granularity (FAT: 2 s) and NFS
attribute caching make it best-effort.

## 5. FilmStrip, MainWindow and the user's view

**Activation that waits for its folder.** Today `openFile` lists synchronously and then calls
`shotContaining`. That breaks once listing is asynchronous:
- `FilmStrip::setFolder(folder, std::optional<fs::path> activate = {})` stores `pendingActive_`, a
  file (primary or companion). On each `rowsInserted`, if `shotContaining(pendingActive_)` succeeds,
  call `setActive` (no `activationRequested`: the photo is already open) and clear the pending
  activation. On `listingChanged(false)` without a match, clear it (the B3 outcome: the photo is open
  and no shot is active).
- `MainWindow::openFile`: if the strip's folder is an ancestor of the file and a walk is running,
  pass the file as the pending activation instead of calling `setFolder(parent)`. Otherwise keep
  today's logic.
- `openInitialPath` uses the same path: `setFolder(root)`, then `openFile` sees "listing under root".
- `openFolder` activates the first visible shot today. New signal `firstShotListed(QString)`, sent
  once per generation. The window activates it unless the user has already clicked something.
  `rememberFolder` runs at `setFolder` time, because the root check has passed.

**Busy note.**
- After 250 ms of listing (the same anti-flicker rule as the render progress plan), the folder label
  reads `~/Photos (+ subfolders) — listing… 12 345 shots`. It updates at most every 250 ms and is
  cleared by `listingChanged(false)`.
- While the strip is empty and listing, `emptyHint_` reads "Listing folder…". Today the hint only
  says "No shots match the filter". Empty and finished, it reads "No photographs in this folder".
- No determinate bar: the total is unknown until the end.

**Notice.** One `QToolButton` in the title bar, right of the folder label, `autoRaise`, with
`SP_MessageBoxWarning` and text. Clicking it is F5. It is hidden when nothing applies. Text by
priority:

| Condition | Text | Tooltip |
|---|---|---|
| The root is unavailable | "Folder unavailable" | "%1 cannot be read (%2). The strip shows what was last listed. Refresh (F5) to try again." |
| k subfolders unreadable | "%n folder(s) could not be read" | The first few paths and their errors. |
| N > 0 unwatched | "%n folder(s) not watched" | "Arraw watches at most 1000 folders, the shallowest first. Changes in the others show after Refresh (F5)." |
| Over 100 000 shots | "Large folder: %n shots" | "Very large listings can make the strip slow." (Shown once per listing.) |

It clears when the condition clears, through a `watchStatus` with 0, a new folder, toggling
subfolders off or a successful refresh. The status line gets nothing: the strip is a dock and can be
seen on its own, and status messages time out.

**F5.** `docs/keybindings.md` has no F5, and `Ctrl+R`/`Cmd+R` is unused.
- Add **View > Refresh Folder** with `QKeySequence::Refresh` (F5 on Windows/KDE, F5 and Ctrl+R on
  GNOME, Cmd+R on macOS; check this on each platform). Following the wiring rules, also
  `addAction` it on the window, so it works with the menu bar hidden.
- Disabled without a folder. Add a row to *Current* in the same change.

**Selection across a reset fallback** (2, the 64-run case).
- On `modelAboutToBeReset`, stash the selected primaries, the current primary and the first visible
  primary.
- On `modelReset`, restore them by `proxyIndexOf`, and `scrollTo` the anchor.
- `listFolder`'s `thumbnails_.setShots` stays only in `setFolder`. A merge reset calls `addShots`,
  which does not double.

**Toggle "Include subfolders" without a reset** (step 6). The root's span is first in order:
- **On:** the lister walks the root's children. Their spans are appended after the root's span, so
  the selection, the marks and the loader stay.
- **Off:** one `beginRemoveRows` of every row after the root's span, and the lister releases the
  subtree's watches.
- Neither can fail now that the root is known to be readable, so the revert branch in
  `setIncludeSubfolders` goes.
- The loader generation is **not** bumped. The lister walk carries its own walk number, so a stale
  walk's batches are dropped while the root rows' pending marks still land. R5 (the active shot in a
  subfolder) keeps the existing guard.

## 6. Failure modes

| Case | What happens |
|---|---|
| Root missing or unreadable at `setFolder` | The synchronous root check throws, as now. Dialog or status line, as now. |
| Root vanishes later (unmount, rename, NAS asleep) | **Rows are kept.** The notice reads "Folder unavailable" and F5 retries. Today the strip is emptied ("counts as empty"), which on a NAS hiccup throws away 50 000 rows and minutes of marks and EXIF reads. *Open question 1.* |
| Subfolder gone (ENOENT, ENOTDIR) | `removeSubtree`: one contiguous removal. Its watches are released. |
| Subfolder EACCES (permission changed) | Treated as gone: removed, and counted in "could not be read" until the next walk. This is deterministic and matches what a fresh listing shows. *Open question 2.* |
| Subfolder EIO, ENOTCONN, ESTALE or a timeout | **Span kept, marked stale**, counted in the notice, retried on its next event or on F5. |
| Slow or cold share | Nothing blocks the GUI thread. The strip fills as directories come. Cancellation waits for the `readdir` under way. |
| A `readdir` that hangs (hard-mounted NFS) | The lister is stuck. New folders queue behind it, and the app hangs at exit when the thread is joined. Mitigation (later, *open question 6*): on a new generation, if the step under way has run more than 5 s, orphan that thread (its delivery goes through a `QPointer` guard and a `shared_ptr` state) and start a fresh lister. At exit, wait with a timeout. |
| `addPath` refused (system limit) | Counted as unwatched, so the notice shows. |
| The loader reads a file whose folder was just removed | Already harmless: `receive` ignores an unknown primary. `pending_` still counts down. |

## 7. Testing

**Pure units, no Qt event loop.**
- `diffFolder`: runs, updates, a primary change (JPEG-only → RAW+JPEG), 2 000 interleaved inserts →
  run count.
- `WatchBudget`: shallowest-first, eviction, promotion, ENOSPC shrink, determinism.
- `folderPrecedes` and the pre-order equivalence: `listShots(recursive)` before and after the
  re-implementation agree on generated trees (`Rome`, `Rome 2`, `Rome/raw`).
- `listFolder` error codes (ENOENT and EACCES; skip EACCES when running as root).

**ShotModel, synchronous.** Feed `FolderResult`s directly:
- appends during a walk;
- a mid-tree insert of a new folder;
- `removeSubtree`;
- a stale generation dropped;
- `loadingFinished` only after `finishListing`;
- rechecks only for the applied folders (count the loader jobs through a test-only accessor, or
  `pendingLoads`);
- the reset fallback above 64 runs.

**Lister and watcher, real threads, deterministic.**
- Debounce is injected (the constructor already takes it). Tests use 0 ms, plus a test-only
  `flushDirty()`.
- `simulateDirectoryChanged(path)`, invoked on the lister thread, drives the logic without depending
  on inotify timing.
- Waiting is always on a condition (`QTest::qWaitFor` or the existing `waitUntil`) on a signal or
  state that proves the step: `listingChanged(false)`, `watchedDirectories().contains(d)`, a
  `QSignalSpy` on `rowsInserted`. **No fixed sleeps.** This replaces the 200 ms wait of R8.
- A handful of real-inotify integration tests:
  - a file in a watched subfolder;
  - a new subfolder with files written immediately (the R7 race, looped about 50 times);
  - a removed subtree;
  - a promoted directory re-listed.

**FilmStrip, widget tests.**
- pending activation (open a deep photo while the walk runs);
- `firstShotListed`;
- the notice text and its clearing;
- F5 merges and keeps the selection;
- the toggle on/off without a reset.

Give the notice and the toggle an `objectName` (review N12).

**Benchmark (not in the default suite).** `tests/test_FolderListingBench.cpp`, tags
`[.bench][listing-bench]` (the pattern of `test_BrushBench.cpp`).
- It generates 200 × 250 shots (ARW+JPG+XMP, empty files) in a temp dir, or uses
  `ARRAW_BENCH_TREE` for a real tree or a NAS.
- Reports:
  - `listShots` recursive, against the old one at step 1;
  - time to the first batch, and to `finishListing`;
  - the **longest GUI-thread stall**, measured by a 0 ms `QTimer` heartbeat with `QElapsedTimer`
    gaps;
  - the per-directory refresh after touching one file;
  - an interleaved copy of 2 000 shots.
- Targets: first batch under 100 ms locally, no GUI stall over 50 ms at 50 000 shots, and a
  one-folder refresh under 30 ms.

## 8. Steps

Each step is a commit that builds, passes the suite and can ship on its own.

| # | Step | Shot.h / Shot.cpp | ShotModel | FolderWatcher | FilmStrip (+ MainWindow) | AppSettings | Effort |
|---|---|---|---|---|---|---|---|
| 0 | Land the working-tree fixes (B1, R3, R4, B2, B3) | (in tree) | (in tree) | (in tree) | (in tree) | (in tree) | — |
| 1 | **Core primitive and benchmark** | `FolderListing`, `listFolder`, `folderPrecedes`, `filesystem_error` with codes, cached entry types; `listShots` as a pre-order DFS over `listFolder` | — | — | — | — | S, ½ day |
| 2 | **Linear merge (R2), still synchronous** | — | `spans_`, `diffFolder` (new `FolderDiff.h`), runs, tail reindex, reset fallback; `refresh` re-lists synchronously but merges per folder and rechecks only changed folders (from the per-folder diff) | — | Selection stash and restore around a model reset | — | M, 1 day |
| 3 | **Background walk** (option A without watching yet: the lister walks, and `FolderWatcher` keeps its `QFileSystemWatcher` but takes the directory set from the walk, so no second walk, R1 b) | — | `setFolder` = root check + reset; `applyFolders`, `finishListing`, `isListing`, `generation`; `refresh` → asks the watcher | Owns the `FolderLister` thread; starts walks on `modelReset`; rewatch = diff of the watch set (no remove-all/add-all) | Pending activation, `firstShotListed`, busy note and empty-hint texts; MainWindow `openFile`, `openFolder`, `openInitialPath` | — | L, 2-3 days |
| 4 | **Per-directory refresh, watch-before-list** (the watcher moves into the lister) | — | `removeSubtree`; rechecks per folder | Lister owns `QFileSystemWatcher`, dirty set, debounce; `FolderWatcher` keeps `ownStamps_` and the sidecar judgement and relays status | — | — | L, 2 days |
| 5 | **Budget, notice, F5** | — | Rechecks for all rows on a manual refresh | `WatchBudget` (new), eviction and promotion with re-list, ENOSPC; `refreshAll()`; `watchStatus` | Notice button; MainWindow View > Refresh Folder (`QKeySequence::Refresh`, window-level); `keybindings.md` row | — (optional: hidden `filmStrip/maxWatchedFolders` override, *open question 7*) | M, 1-1½ days |
| 6 | **Incremental toggle, failure polish, 100k note** | — | `setRecursive` without reset; root-unavailable keeps rows; stale spans | Toggle and walk-number plumbing; error classification to statuses | Toggle without revert branch; "Folder unavailable" and "could not be read" notice texts; large-listing note | — | M, 1-1½ days |
| 7 | **ADR and docs** | Header docs | Class doc (no longer "made at once") | Class doc | — | — | S |

- Step 3 is the biggest risk and the biggest win: the freeze goes. If it has to be split, ship 3a
  (lister and model API, with `FilmStrip`/`MainWindow` waiting in a nested `QEventLoop` until
  `finishListing` as a temporary shim, so behaviour stays synchronous) and then 3b (pending
  activation and the busy UI, shim removed).
- Steps 4 and 5 can swap order if the notice is wanted sooner. The notice then reports today's BFS
  budget.
- Suggested ADR 052: "The film strip lists in the background, folder by folder, and refreshes the
  folder that changed."
- Models (CLAUDE.md):
  - Opus with high thinking for steps 1, 3 and 4 (the threading and ordering contracts).
  - Sonnet for 2, 5 and 6 and the tests.
  - An Opus review after 3 and after 5.

## Before and after (50 000 shots, ~200-2 000 folders)

| Event | Now | After |
|---|---|---|
| Open folder | ~0.5-0.9 s GUI freeze (local), tens of s (SMB) | First shots in < 100 ms; no stall > 50 ms; walk time unchanged but in the background |
| Rate a shot (sidecar write) | Whole-tree walk ×2 + 50 000 sidecar stats | One `readdir` of that folder + its rows' stats |
| Copy 2 000 shots into a new folder | Seconds (R2) | One walk of that folder, one insert run |
| Toggle subfolders | Full synchronous re-list, reset | Background walk of the children, appended; off = one removal |
| Change beyond 1000 watched folders | Seen on the next unrelated refresh | Seen on F5; notice says so |

## Risks

- **`QFileSystemWatcher` on a worker thread.** Supported, but the Windows engine's handle threads
  and the macOS FSEvents engine are untested here (R6). Qt's silent dropping of a watch on a move or
  delete of the directory itself is handled by rule 5, but needs a test per platform. The fallback is
  option B with 4b.
- **One comparator.** If the core DFS order and `FolderOrder` ever disagree, spans are inserted in
  the wrong place, rows can interleave and the subtree removal removes the wrong range. Both must
  call `arraw::folderPrecedes`, and a debug-build assertion should check that the span map stays
  sorted by first row.
- **Proxy cost.** `QSortFilterProxyModel` does O(N) work per source insert or remove. Runs and the
  reset fallback bound it, and the benchmark watches it.
- **Generations.** Two numbers (folder generation for the loader, walk number for the lister after
  step 6) are easy to confuse. Tests must cover a stale batch arriving after a toggle and after a new
  folder.
- **Hung syscalls** (6): exit can hang on a dead hard-mounted NFS until the orphaning mitigation
  lands.
- **Behaviour changes callers rely on.** `setFolder` no longer fills the model before returning.
  About 25 test call sites and three MainWindow paths assume it does. Step 3 must update them (or use
  the 3a shim).

## Open questions for the user

1. When the root folder becomes unavailable (a NAS asleep or unmounted), keep the rows with a
   "Folder unavailable" notice (recommended), or empty the strip as now?
2. When a subfolder becomes unreadable through permissions (EACCES), remove it (recommended), or keep
   its rows as stale like an I/O error?
3. During a long import, should the strip update every ~2 s while files keep arriving (maximum
   latency), or only once the copy pauses (as now)?
4. Should opening a folder keep its synchronous root check, so an unreadable folder is still a
   dialog at once (recommended)? The alternative is fully asynchronous, with the error arriving
   later in the strip's notice.
5. The 100k warning: count shots (recommended) or files? Is the strip notice enough, or should it
   also show in the status line?
6. Is orphaning a stuck lister thread (and not waiting for it at exit) acceptable, or should a hung
   share simply be documented?
7. Should the 1000-watch cap be a hidden setting (`filmStrip/maxWatchedFolders`) for users who raise
   `fs.inotify.max_user_watches`, or a constant?
8. "Refresh Folder": in the View menu (recommended) or the File menu next to Open Folder? And should
   the strip's context menu have it too?
9. Is a periodic poll of unwatched folders (for example a stat of each one every 60 s) still out of
   scope, given decision (4)?
