# Review: film strip "Include subfolders", XMP sidecar badge and file-name caption

Reviewer: claude-opus-5-5, 2026-10-19. Branch `lens-corrections`, commits 96cd62a (sidecar badge and
the "Include subfolders" toggle) and b2a92d3 (file-name caption), so
`git diff 18591ec..b2a92d3 -- src include tests` (16 files, +730/-34). No production code or tests
were changed for this review.

## Verdict

**Mergeable for a local, medium-sized library. Fix B1-B3 before turning the toggle on by default or
advertising it for large or NAS trees.** The core walk, pairing per folder, the sidecar-role split
and the delegate are sound, and the new tests pass. There are three real bugs:
- The watcher spends its 1000-directory budget on NAS clutter that the listing skips.
- A photo opened from a subfolder becomes the "last folder", so a restart drops back to that
  subfolder.
- "Already listed" is judged by path prefix, not by what the strip actually holds.

The main risk is cost. On every debounced change, the GUI thread re-walks the whole tree and walks
it again for the watch.

## Findings

### Bugs

**B1. The watch walk enters `@eaDir`, `#recycle` and the other clutter that the listing skips.**
`src/app/FolderWatcher.cpp:28-54` (comment at :27 says "as ::arraw::listShots leaves them").
- `directoriesToWatch` filters only hidden names and symlinks. `isJunkFolder` (`src/core/Shot.cpp:159`)
  is file-local to core and is not used here.
- Scenario: a tree copied off a Synology keeps an `@eaDir` in every folder, with **one
  sub-directory per photo** (`@eaDir/IMG_0001.JPG/SYNOPHOTO_THUMB_*.jpg`). With about 1000 photos,
  the BFS's level 2 is mostly `@eaDir/<photo>` entries. The 1000-watch cap is hit before real
  folders at depth 2-3 are watched. Which real folders miss out depends on `directory_iterator`
  order, which is not sorted, so it varies from one rewatch to the next.
- A side effect: changes inside the clutter (an indexer, the recycle bin) start full refreshes of
  the strip.
- Fix: move the "do not enter" rule (hidden, junk, symlink) into one shared private helper (for
  example `src/core/ShotFolders.h`, or an `arraw::isListedFolderName` in `Shot.h`) and use it in
  both walks. Better still, see R1: have the listing return the directory set, so the watcher
  needs no second walk.

**B2. With subfolders on, the "last folder" becomes the photo's subfolder, so a restart lists the
wrong root.** `src/app/ui/MainWindow.cpp:1655` and `:2266` (`showPhoto` → `rememberFolder(path.parent_path())`),
together with `src/app/AppSettings.cpp:13-25`.
- Scenario: open `~/Photos/2024` with subfolders on and activate `2024/Rome/IMG_1.ARW` in the strip.
  `lastFolder` is now `2024/Rome` and `lastFile` is that photo.
- On restart, `restoreOpenPath` returns the file, because its parent equals `lastFolder`.
  `openFile` then sees an empty strip, so `listed` is false, and calls `setFolder(2024/Rome)`.
  The strip now holds one folder instead of the tree. The Open Folder dialog also starts in
  `Rome`.
- The `showPhoto` call already existed. It only became wrong now that a photo's parent can differ
  from the strip's folder.
- Options:
  - (a) `rememberFolder(filmStrip_->folder())` whenever the photo is listed.
  - (b) Store the strip's folder separately from the photo (for example `filmStrip/folder`) and
    have `restoreOpenPath` restore both.
  - (c) Accept the behaviour and document it.
  - I recommend (a) as the smallest change. (b) is the right one if more strip state gets persisted.

**B3. "Below the listed folder" is decided lexically, so photos the listing skips are treated as
listed.** `src/app/ui/MainWindow.cpp:1634-1642`.
- Scenarios, all with the strip on `~/Photos` and subfolders on:
  - File > Open `~/Photos/Archive/IMG.ARW`, where `Archive` is a symlink to a NAS folder (links
    are not followed).
  - Opening a file under `.hidden/` or `@eaDir/`.
  - Opening a file just copied into a new folder that is beyond the watch cap and not yet
    refreshed.
- In each case `listed` is true, so the strip is not changed. `shotContaining` then fails, and
  `clearActive()` runs.
- The result: the photo is open, nothing in the strip is active, and the rating and label actions
  are disabled (`CullingActions::reflectActiveShot`, `CullingActions.cpp:108-116`). Before this
  change the strip switched to the photo's folder.
- Fix: decide by `filmStrip_->shotContaining(path)`. If the shot is not found, fall back to
  `setFolder(folder)`, or refresh once and look again.
- This branch was added beyond what was asked (the brief named the toggle). The intent is good,
  but it needs this guard.

**B4. Two new ShotModel tests fail on Windows.** `tests/test_ShotModel.cpp:476` and `:488` expect
`"a/IMG_1.dng"` and `"day1/raw/IMG_0042.dng"`. `shownName` → `qtString` (`ShotModel.cpp:31`, `:316`)
produces native separators, so Windows gives `a\IMG_1.dng`. Decide what the tooltip should show
(native separators fit Windows users), then either compare with `QDir::toNativeSeparators` in the
test or use `generic_u16string()` in the model.

### Risks

**R1. A full synchronous re-walk, plus a second walk for the watch, on the GUI thread for every
debounced change. Neither is cancellable and neither has a cap.**
`ShotModel::refresh` (`ShotModel.cpp:321-366`), `FolderWatcher.cpp:64-69`, `:91`.
- Measured: I re-implemented `filesUnder` and the BFS exactly, built with `-O2`, on a warm local
  ext4 tree of 200 folders × 250 shots (ARW+JPG+xmp, 150 000 entries):
  - `filesUnder` plus bucketing took **450-630 ms**.
  - `directoriesToWatch` took **240-290 ms**, even though it only needs about 200 directories,
    because it visits and stats every file entry.
  - That is about 0.7-0.9 s per refresh before `groupShots`, the diff and the 2×N loader jobs. The
    debug build is slower. A cold cache, an HDD or SMB/NFS is one to two orders of magnitude
    slower (about 0.1-1 ms per stat over SMB, so tens of seconds for this tree).
- Triggers:
  - Every app sidecar write (rating with the keyboard, Save), because the directory changes and
    the 300 ms trailing debounce fires.
  - Every import step once the copy pauses.
  - The toggle itself.
  - App start, because the flag is persisted. Choosing `~` or a whole-library root with the flag
    on freezes the window with no way out.
- Options:
  - (a) Run `listShots` on the loader thread (or a `QtConcurrent` task). Drop the result by
    generation, as the marks jobs already do. Apply the diff on the GUI thread.
  - (b) Have the walk return the directory set as well (`listShots(..., std::vector<path>* folders)`
    or a `Listing{shots, folders}`), so `rewatch` does no walk of its own. Diff the watch set
    instead of a `removePaths(all)`/`addPaths(all)` round trip.
  - (c) Cap the walk (shots or depth) and say so in the label ("+ subfolders, first 20 000").
  - (a)+(b) is the real fix. (c) is a cheap safety net.

**R2. `refresh`'s diff is quadratic, which only matters now that N can be tens of thousands.**
`ShotModel.cpp:338` (`erase` per row), `:354` (`insert` per row), `:360`
(`std::ranges::find(added, …)` per loaded row).
- Scenario: a 2 000-photo folder is copied into a 48 000-shot tree. That costs about 48k × 2k ≈ 10⁸
  path comparisons in the recheck filter, plus 2 000 mid-vector `Row` moves of 48k rows, plus 2 000
  `beginInsertRows` passing through `QSortFilterProxyModel`. That is seconds on the GUI thread
  (an estimate; it was not run in the model).
- The same happens after R3 truncates a listing and the next refresh puts the rows back.
- Fix:
  - Use an `unordered_set` for `added`.
  - Merge into a new vector and use `beginResetModel` when more than about 10 % of the rows change.
  - Or insert runs of rows with one `beginInsertRows` per run.

**R3. Any error other than "permission denied" ends the whole walk silently, despite the comment.**
`src/core/Shot.cpp:210-214` ("A subfolder that cannot be read ends the walk of that branch only"),
and the header (`include/Shot.h`: "a subfolder that cannot be read is skipped").
- In libstdc++, `recursive_directory_iterator::increment(ec)` resets the iterator to end on any
  error, and the code then `break`s. So the rest of the tree is dropped, not only that branch.
  `skip_permission_denied` covers only EACCES.
- Scenario: a subfolder that is deleted or renamed during the walk (ENOENT), a disconnected
  sshfs/SMB mount (ENOTCONN/EIO), or a stale NFS handle. `refresh` then removes every row after
  that point, the active row possibly among them. The next refresh puts them back (R2 cost).
- Options:
  - (a) Walk with an explicit stack of `directory_iterator`s and skip a directory that fails.
  - (b) If the walk ended early, throw, or return a "partial" flag that `refresh` treats as "keep
    the old rows".
  - At minimum, correct the comment and the header.

**R4. The order is not depth-first: a folder's subfolders are not next to it.**
`src/core/Shot.cpp:234-241` compares whole path strings, so the separator competes with other
characters. `' '`, `'-'`, `'.'`, `','` and `'('` sort before `'/'` (and before `'\\'` on Windows).
- Scenario: folders `Rome`, `Rome/raw` and `Rome 2` list as `Rome`, `Rome 2`, `Rome/raw`.
  `Trip`, `Trip-2` and `Trip/day1` behave the same way. In the strip this reads as interleaving.
- The header promise ("natural order of the folder paths") is met literally, but this is
  surprising. Linux and Windows also order differently.
- Fix: compare component by component with `compareNatural`, which keeps the root first and
  subtrees contiguous.

**R5. Turning the toggle off while a subfolder photo is open leaves an open photo with no active
shot.** `src/app/ui/FilmStrip.cpp:482-512`.
- This is as designed and tested ("the active shot in a subfolder goes with it"). The consequence:
  the editor keeps the photo, but rating and label shortcuts go disabled, the same outcome as B3.
  The next arrow key jumps to the first root shot.
- Options:
  - (a) Keep it as is.
  - (b) When the active shot would drop out, list the active shot's folder instead.
  - (c) Keep the active shot as an extra row.
  - This is a question for the user, not a defect.

**R6. Windows watches (unverified).** With 1000 directories, Qt's Windows engine holds about 1000
change-notification handles (about 16 threads at 64 per thread). Watched directories are known to
be hard to rename or delete from Explorer while they are watched. This should be checked on
Windows before the cap is relied on there. Linux inotify is fine at 1000. The macOS FSEvents
engine was not checked.

**R7. Race in `refresh` then `rewatch`.** `FolderWatcher.cpp:64-69`.
- `rewatch` removes every watch and adds them again after the listing. A file written into a
  watched folder between `removePaths` and `addPaths` is missed.
- The same happens for a file written into a brand-new folder after `listShots` read it but before
  the folder was watched.
- It shows only on the next unrelated change. Watching first and listing second closes the gap.
  Diffing the watch set (R1 b) removes the remove/add window.

**R8. The fixed 200 ms wait in the watcher test can pass when it should fail. It does not cause
spurious failures.** `tests/test_ShotModel.cpp:507-508`.
- `waitUntil(rowCount()==2)` is already true when it is reached (`sub/IMG_2` made it 2), so it
  waits for nothing. The real synchronisation is the bare 200 ms.
- On a slow machine the refresh for `later/` can run after `IMG_3` exists. `listShots` then finds
  the file, and the test passes even if the recursive rewatch were broken.
- Measured: 50/50 passes idle and 50/50 under 24 busy loops on 12 cores. The test is not flaky.
- Fix: wait on a condition that proves the watch, for example a test-only accessor for the
  watched directories, or `QSignalSpy` on `modelReset`/`rowsInserted` after a probe file.

### Nits

- **N1. Cost of the BFS** (`FolderWatcher.cpp:45`). `symlink_status` on every entry, files
  included. `entries->is_symlink()` and `is_directory()` from the cached `d_type` would avoid the
  `lstat` on most filesystems. Better still, see R1 b.
- **N2. Junk list** (`Shot.cpp:159-162`).
  - Names are case-sensitive. The Windows system drive has `$Recycle.Bin`.
  - Synology's `#snapshot` and QNAP's `@Recycle`/`@Recently-Snapshot` are missing.
  - The header lists three names, but the code has four (`System Volume Information`).
  - Windows "hidden" attributes are ignored, the same as in the flat listing.
- **N3. Bind-mount loops are not detected** (only symlinks are refused). This is rare. A
  `(dev, inode)` visited set would be cheap insurance once R3 (a) uses an explicit stack.
- **N4. `listShots(path, bool)`** puts a bare `bool` in the public API, and the CLI
  (`src/cli/ShotInputs.cpp:33`) and Python (`BindShots.cpp:50`, `_arraw.pyi:2226`) do not expose
  it. Consider an options struct or enum (`Subfolders::Include`). Add `recursive=False` to
  `list_shots` if scripting should get it.
- **N5. Badge below `marksMinHeight`** (`FilmStrip.cpp:118-140`). Cells under 64 px hide the stars
  and the chip as illegible, but they still paint a 12 px badge. Hide it there too, or say why it
  is the exception. I checked positions at 32, 64 and 120 px: there is no overlap with the swatch,
  the stars bar or the format chip.
- **N6. Selected caption** (`FilmStrip.cpp:226-232`). The strip, filled at α 0.6, and the 4 px
  outline overlap by about 1-2 px at the bottom and left, so a selected (not active) cell has a
  darker seam. Inset the strip by `borderWidth + 1`, or fill before the outline with
  `CompositionMode_Source`. Active cells are opaque, so they are unaffected.
- **N7. Duplicate names are indistinguishable.** With subfolders on, two `IMG_0001.ARW` show the
  same caption, and only the tooltip tells them apart. Consider `day1/IMG_0001.ARW` with
  `ElideLeft` when the name is not unique, or a folder separator chip.
- **N8. Dock height** (`MainWindow.cpp:1738`). `resizeDocks({stripDock_}, {132})` was not updated,
  so the default thumbnails shrink by the caption height (about 120 → about 100 px of content).
  Use `filmStrip_->sizeHint().height()`.
- **N9. Toggle UI** (`FilmStrip.cpp:365`, `:477`).
  - `SP_FileDialogDetailedView` is the "detail view" icon, not a folder-tree icon.
  - `tr(" (+ subfolders)")` puts the leading space inside the translatable string.
  - A failed re-list pops the button back with no message. Pass the error to the status line, as
    `openFile` does.
- **N10. Double stat** (`FilmStrip.cpp:707-708`). `noteOwnWrite` → `noteSidecarWritten`, followed
  by `setMarks` → `noteSidecarWritten`, stats each sidecar twice on the GUI thread. This is
  harmless, but `setMarks` could leave it to the caller, or `noteOwnWrite` could skip it.
- **N11. Companion sidecars never show the badge.** `saveAdjustments` notes
  `session.photo().path()`. For a developed JPEG companion that path is not a row, so the badge
  (primary-only) ignores it. That is probably intended, but it is undocumented in
  `HasSidecarRole`.
- **N12. Tests.**
  - `test_ShotModel.cpp:447`: the comment "Without a file, setting marks shows no sidecar" sits
    above a call for an *unknown* primary. It belongs two lines lower.
  - `test_FilmStripSubfolders.cpp` finds the button by its tooltip text, which is translatable.
    An `objectName` would be sturdier.
- **N13. Shared QSettings in the widget tests.** `MainWindow` reads `filmStrip/includeSubfolders`
  from the test-mode store (`~/.qttest/config/arraw-tests/…`), which persists across runs. No test
  writes it today. A future window test that clicks the toggle would turn recursion on for every
  later `MainWindow` test and run. Remove the key in such a test, or give MainWindow an injected
  `QSettings`.

### Untested

- The MainWindow wiring: restore on construction, save on toggle, the "below" branch of
  `openFile` (B3), and `rememberFolder` (B2).
- The 1000-directory cap and the BFS order. No test covers junk directories in the watch (B1).
- A walk that fails mid-tree (R3). The ordering of `Rome`/`Rome 2`/`Rome/raw` (R4).
- Delegate painting of the badge and the caption, beyond "cells are taller than wide". The test
  can save a screenshot (`ARRAW_TEST_SCREENSHOT_DIR`), but it asserts no pixels.
- `FilmStrip::noteOwnWrite` → badge (only the model-level call is tested).

## Verified OK

- **Symlink loops.** `recursive_directory_iterator` without `follow_directory_symlink` never enters
  a link (tested), and the BFS skips links. Symlinked *files* are still listed, as in the flat
  listing.
- **Pairing per folder.** It is bucketed by `parent_path` in `groupShots` and again per folder in
  `listShots`. The same stem in two folders gives two shots (tested). The order within a folder is
  unchanged.
- **Error at the root.** A missing or unreadable root still throws with its name (tested). A flat
  listing is byte-for-byte the old path.
- **Hidden and junk.** Hidden files are dropped. Hidden and junk directories are pruned with
  `disable_recursion_pending` before descending (tested for `.x` and `@eaDir`).
- **Generations.** `setFolder(folder, recursive)` bumps the generation and clears the loader for a
  same-folder toggle. Stale marks, EXIF and rechecks are dropped before `--pending_`. `modelReset`
  re-points the watcher with the new flag. `rewatch` stops the debounce.
- **The `sidecarExists`/`sidecarStamp` split.**
  - The role is false until marks are read.
  - A Marks result does not overwrite a hand-set row's existence.
  - A recheck to stamp 0 clears it.
  - `noteSidecarWritten` re-stats without touching the stamp, so the next recheck still reads the
    file, and `ownStamps_` keeps it from being reported as external.
  - `dataChanged` carries `HasSidecarRole` and `ToolTipRole` only on a transition (tested).
- **Toggle semantics.**
  - It is listed first. A failure reverts the flag and the button with signals blocked.
  - The active shot is kept when it is still listed, and `activeChanged` is emitted either way.
  - The flag carries over to later `setFolder` calls (tested).
  - The restore at construction runs before the save is connected, so start-up does not write
    the setting back.
- **AppSettings.** Separate functions under `filmStrip/`. `saveAppSettings` leaves the key alone
  (tested with an INI store).
- **Caption layout.**
  - The caption height comes out of the thumbnail side and is dropped, together with the caption,
    when the content would fall under 64 px. `paint`'s guard matches that rule.
  - The strip height is the font height plus 2 px.
  - `ElideMiddle` is applied within the strip width.
  - HiDPI: the thumbnail scales by the DPR, and the font is in points.
- **Build and tests.** `ARRAW_BUILD_PREFIX=container- just build` had no work to do (it was
  current).
  - The new `arraw-tests` cases pass, and so do the `[shotmodel]`, `[shot]` and `[app]` cases
    (266 cases).
  - `arraw-widget-tests "[filmstrip]"` passed 60/60.
  - Full `arraw-widget-tests` passed 6 of 7 runs. One run reported 1 failed assertion, which I
    could not reproduce or identify in the six reruns. It is not attributed to this change.
  - `just format-check` is clean.

## Open questions for the user

1. Toggling off while a subfolder photo is open: clear the active shot (now), switch the strip to
   the photo's folder, or keep the shot? (R5)
2. Should the app remember the strip's root folder separately from the last photo? (B2 b)
3. Is a cap on recursive listing acceptable ("first N shots" with a note), until the walk moves off
   the GUI thread? (R1)
4. Tooltip paths on Windows: native `\` or `/`? (B4)
