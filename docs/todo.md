# To-do

As of 2026-10-07, branch `copy-settings`, nothing pushed. Earlier, on
`finish-global-adjustments`: the global-adjustments plan, phases 1–7
(`docs/ideas/global-adjustments-plan.md`), the crop and rotation tool
(`docs/ideas/crop-ui-plan.md`), and render progress with cancellation
(`docs/ideas/render-progress-plan.md`). The reviews are in `docs/reviews/`,
untracked on purpose.

## Done: demosaic as settings of the negative (branch `negative-settings`, 2026-10-16)

Phase 1, steps 1-9 of `docs/ideas/negative-settings-plan.md` (ADR 049, Option C):
- `NegativeSettings` (`include/NegativeSettings.h`), held by `Photo` beside
  `DevelopState`; the demosaic is one of LibRaw's seven LGPL built-ins, saved as a
  token, AHD by default. No develop edit can reach it, by type;
- `DecodeOptions.demosaic`, `canonical`, `decodeOptionsFor(photo, halfSize)` and
  `ImageBuffer::decodedWith()`; `ProcessingPlan::decode` is a plan header, so a
  new negative is a stale rung by value (ADRs 005, 012, 045 amended);
- history, unsaved changes and the sidecar's `arraw:demosaic` (ADRs 022, 030);
  `info --json` and Python carry `"negative"`;
- copy/paste: `CopySection::Demosaic`, in the default sections, with a migration
  of remembered selections; paste applies the negative as one history step;
- the app: a Negative group on the Adjustments tab, a re-decode off the GUI
  thread with the old picture kept, the export queue deciding per job;
- the CLI's `export --demosaic`, Python's `Demosaic`, `NegativeSettings`,
  `set_negative`.

Where the code differs from the plan:
- `canonical()` also resets `halfSize` for a non-RAW, not only the demosaic;
- `ImageMetadata::mosaic` knows one non-Bayer pattern, X-Trans; every other
  filter is Bayer, and LibRaw's own substitutions are not detected;
- the review is `docs/reviews/claude-opus-5-5_2026-10-16_negative-settings.md`
  (untracked); its critical and medium findings are fixed (decode loop, retry
  after a failed re-decode, export queue using `DecodeOptions`).

Status (2026-10-17): Phase 1 is done and re-reviewed merge-ready (ADR 049
accepted); committed on `negative-settings`, not pushed or merged. Open:
- low: discarding changes while closing the window starts a pointless
  re-decode; X-Trans / four-colour re-decodes, the LibRaw mapping pin test and
  the thumbnail key (Phase 2) are in the review;
- `docs/photo-development.pdf` is stale against section 3.4 (no PDF recipe);
- `PresetStore` (looks plan step 4) must carry the negative (plan step 6);
- Phase 2, lens corrections: done on `lens-corrections` (below);
- known flaky under load: the brush repaint app tests, "An observed loop
  covers every row once".

## Done: lens corrections (branch lens-corrections, 2026-10-18)

Steps 0 to 10 of `docs/ideas/lens-corrections-plan.md` (ADR 050, accepted), not
committed or pushed:
- `NegativeSettings::lens`: three corrections (off by default), a vignetting amount (0-150, 100 is the profile) and a recorded lens
  (lensfun maker and model, crop factor, origin, database stamp); RAW only;
- `Stage::Lens` between Denoise and Pointwise: distortion, lateral CA and
  vignetting in one Catmull-Rom pass from radial tables of 129 nodes, with a
  fill scale; a toggle resumes from the Denoise rung and decodes nothing;
- lensfun matched from the EXIF (camera, then lens, scored, filtered by focal
  range and aperture, ambiguity applies nothing) and the database loaded once;
  `ARRAW_WITH_LENSFUN` is `AUTO|ON|OFF`, `ON` on Linux release and CI;
- the sidecar, JSON, history, `CopySection::Lens`, the app (switches, "Choose
  lens...", status), the CLI (`--lens-database`, `lenses`, `info`), Python,
  the brush through a forward map, and a GPU pass held to the CPU;
- ADRs 009, 012, 022, 030, 031, 044, 045 and 049 amended.

Where the code differs from the plan (also in ADR 050's amendment):
- resolution failures are a `LensProblem` enum, plus `Notice::LensNotCalibrated`;
- `sampleLensfun` takes the database; tables have 129 nodes;
- Python's `lens` argument is optional on the buffer-form calls;
- a paste while the database loads is refused (ADR 050, amendment of 2026-10-19);
  `lensCost` is measured per combination of corrections;
- GPU parity numbers were measured on lavapipe only (no hardware GPU).

Open:
- step 11, the hand check, is mostly done (samples in the git-excluded `samples/`;
  results of 2026-10-18 in `docs/reviews/claude-opus-5-5_2026-10-18_lens-hand-check.md`,
  those of 2026-10-19 not yet written there). Canon 80D, Tamron SP 17-50 f/2.8
  (non-VC): two frames match through Canon's lens-type name (Exiv2 and exiftool agree);
  distortion matches lensfun's model plus the fill zoom to about 1 px. Sony a6400, E PZ
  16-50 f/3.5-5.6 OSS (XYW02870 to 02874, 16 mm, 2026-10-19): the lens matches through
  the alias name; straightness on a picture frame (XYW02872, left edge at radius
  0.75-0.87) goes from 22 px to 2 px of deviation over 2100 px; lateral CA at the left
  edge, R-G -1.6 px without the correction (lensfun predicts -1.46) and 0.0 with it
  (+-0.3 px, one direction only). **Vignetting overcorrects**: arraw applies exactly
  lensfun's gain (within 1-3 %, to r = 0.93) but the profile, taken on a NEX-7, is too
  strong for this a6400 and lens: a flat wall ends 1.26x the centre at r = 0.75 and
  1.35x at r = 0.93 at f/3.5, 1.07x and 1.18x at f/7.1. Done: the vignetting amount slider
  (2026-10-19, ADR 050 amendment: sidecar, JSON, Copy Settings; the session records the
  lens match; review `docs/reviews/claude-opus-5-5_2026-10-19_vignetting-amount.md`).
  Options left: a profile fitted from a
  flat-field shot (XYW02870 is one; larger: a user profile format and a matching rule);
  report the profile upstream to lensfun. Still open: the app toggles, a clipped corner,
  the Tamron 17-70 B070 on the a6400 (in lensfun, to be shot), CA on other edges and
  focal lengths, and the `exiftool -v3` check that a CR2 holds switches only (needs a
  CR2; the 80D is gone, an ARW check is the substitute);
- flaky app test "In the window a stroke repaints only where it painted": FIXED (the test
  now waits for the overlay to go quiet and records the first paint smaller than the
  overlay, ignoring whole-overlay repaints; was pre-existing, ~13 % on a clean checkout);
- Python stub nit (vignetting-amount review S2): `EditSession.lens_database` is typed
  non-optional but is `None` for a PNG, and `vignetting_amount` is `object | None` in the
  stub; give the bindings `nb::sig`s and regenerate (check whether it is still open);
- **default (user, 2026-10-20): lens corrections should be on by default** (now off);
  scope what that means for the sidecar (absent = on or off?), ADR 051 versioning, an
  ambiguous or missing profile (applies nothing, as now), the CLI and Python defaults, and
  existing sidecars that never chose;
- matching hardening: the maker-note name picks the lens by a 6-point fuzzy margin
  (83 against 77 for the VC), so drop candidates carrying tokens the name lacks (VC,
  OS, Macro) or prefer an entry containing all the name's tokens; test first;
- `lens-corrections` is committed (2150c30, 5cec115), not pushed or merged;
- GPU parity on real hardware is unmeasured;
- packaging is unverified: `just rpm`, `rpm-smoke` and the `.deb`'s
  `liblensfun1` dependency were not run here;
- embedded-profile readers (DNG opcodes, Sony, Fujifilm) are later and optional;
- Windows and macOS lensfun are deferred;
- the review, when it lands, is
  `docs/reviews/claude-opus-5-5_2026-10-18_lens-corrections.md` (untracked on
  purpose).

## Done: film-strip background listing (branch lens-corrections, 2026-10-20)

Steps 1 to 6 of `docs/ideas/incremental-refresh-plan.md` (ADR 052, accepted), committed
(29be4c8 to 28a92a9), not pushed:
- core `listFolder`, `folderPrecedes`, `FolderError`; `listShots` as a walk over them;
- `FolderLister` on its own thread, batching to `ShotModel`, which keeps one span per folder and
  merges in linear time (`FolderDiff.h`); no cap on shots or depth;
- per-directory refresh, watching before reading; `WatchBudget` (1000 watches, shallowest
  first), the "N folders not watched" button, View > Refresh Folder (F5, also in the strip's
  context menu), hidden `filmStrip/maxWatchedFolders`;
- an incremental subfolder toggle; a root that cannot be read keeps its rows ("Folder
  unavailable"); unreadable subfolders; the 100k-shot note; a bounded wait (2 s) for a hung
  lister at exit; native path separators in tooltips; the remembered folder is the strip's;
- reviews `docs/reviews/claude-opus-5-5_2026-10-20_background-listing.md` and
  `..._per-directory-refresh.md` (untracked); their findings are fixed in 5c0ae1f and c75ae8d.
  Measured at 50k shots: first rows 7 ms, longest stall 15-27 ms, a new file 13 ms after the
  debounce, F5 2.3 s in the background. Leftovers:
- stalls with a real strip are up to ~0.1 s at tens of thousands of shots (20 000 tiny folders
  49-143 ms; replacing a 50k-row folder 25-98 ms): accepted by the user, 2026-10-20;
- `QFileSystemWatcher` tested on Linux only; SMB, NFS, Windows and macOS unmeasured;
- F5 during a walk is folded into it; a transient watch refusal is not retried until the next
  walk;
- all measures against problems with the strip, by kind of problem, with their tests: ADR 053
  (`docs/adr/053-the-film-strip-guards-against-problems-in-layers.md`);
- the multi-shot film-strip export (GUI tasks below) is to be done after this work.

## Done: histogram panel (branch `histogram`, 2026-10-20)

Steps 1 to 9 of `docs/ideas/histogram-panel-plan.md` (ADR 055), not pushed or merged:
- `OutputHistogram` (core, `include/OutputHistogram.h`): the shown picture as 8-bit codes in the
  output encoding, R, G, B and luma, clipping any channel at 0/255; Python `output_histogram`;
  a GPU-vs-CPU parity test;
- `HistogramCounter` on its own thread (latest wins), the panel at the top of the develop dock
  (collapsible, remembered), a pixel readout, clipping triangles, and the clipping overlay on the
  picture (`J`, View > Show Clipping), red highlights and blue shadows, its state remembered in
  `QSettings` `histogram/clipping`; the whole cropped frame is counted, not the visible part;
- review `docs/reviews/claude-opus-5-5_2026-10-20_histogram-panel.md` (untracked): the critical
  finding (overlay when zoomed) and the mediums are fixed.
Open:
- **Count cost** (user, 2026-10-21): the 10 ms budget is missed, accepted: about 0.1 s of lag at
  4K in a drag is fine, more is not. The counted image is capped at 6 MP by a row/column stride
  (`outputHistogramMaxPixels`; never an extra render): release 59-64 ms at 2560x1600, 31-33 ms at
  3840x2160. Multi-threading the count is rejected: it would take cores from the render in a
  drag. Levers if it grows: a coarser stride only during a drag, a lower thread priority;
- the critical fix (overlay only where a render covers) has had no second review;
- review lows skipped: `TriangleButton` cast and the app-core include of `ThemeColors.h`,
  counting while the window is minimised;
- drag-to-adjust on the histogram (later idea), a log scale, the curve editor's scale left alone;
- the soft cap (third tallest interior bin) wants tuning on real photographs.

## Existing test failure: stroke repaint (found 2026-10-20)

The widget test "While stroking only the ring and the new dabs repaint"
(`tests/test_MaskOverlay.cpp`, ~line 800) fails every time, alone or in the full suite, on `main`
as well as on `histogram`: the overlay repaints whole (right edge 599 of 600, full height) where
it should repaint the ring and the new dabs. It passed earlier the same day, so check what
changed in the environment (display scale, Qt, the 2026-10-20 "In the window a stroke repaints
only where it painted" fix, which made that sibling test ignore whole-overlay repaints) before
treating it as a plain flake. Fix the test or the repaint.

## Done: session history (branch `copy-settings`, 2026-10-08)

Step 3 of `docs/ideas/looks-and-history-plan.md`: `EditSession` keeps a list
and a position, `goTo`, origins and `describeChange` (ADR 022 amended); a
`HistoryModel` and a History dock on the left, where a click navigates;
Python binds `EditSession` with `with session.edit():`. Reviewed in
`docs/reviews/claude-opus-5-5_2026-10-08_session-history.md`. Low leftovers:
- `with session.edit():` commits an edit the block itself began, with the
  scope's origin (L7, second half);
- no window test for the curve's own pending drag then its Reset (N2), nor
  for the dock after a save or a sidecar reload;
- `test_MainWindowHistory.cpp` uses `REQUIRE` inside a `QTimer` lambda (N3).

## Done: copy settings (branch `copy-settings`)

Steps 1 and 2 of `docs/ideas/looks-and-history-plan.md` and the copy/paste
half of its step 4:
- copy sections in the descriptor table; the grain seed belongs to none;
- `include/Edits.h`: one setter per key with its rules (white balance, grain
  seed, geometry), used by the GUI, the CLI and Python (`Photo.edited`);
- `include/CropGeometry.h`: the crop rules, moved from the app into core;
- `withLook` and Edit ▸ Copy Settings… / Paste Settings, Rotate & Flip and
  Crop included but off by default; a pasted crop is normalised and fitted;
- the CLI's `--rotate` and flips carry the crop, `--crop-aspect` alone fits
  inside the sidecar's crop, and a disagreeing `--crop` and `--crop-aspect`
  are reshaped, not refused;
- the crop mode on C, the colour labels on R, Y, G, B, P;
- the render progress as a pie, always in the status bar (ADR 042).

## Done: responsiveness

Opening decodes off the GUI thread (ADR 043), the resize, the geometry, the
zero fill of `ImageBuffer` and export sharpening are banded, the progress
weights were remeasured, and the develop dock keeps every slider at least
120 px wide. Reviewed in
`docs/reviews/claude-opus-5-5_2026-10-06_responsiveness.md`, fixes applied.

## Next

The order and the decisions are in `docs/ideas/architecture-roadmap.md`;
loose ideas are in `docs/ideas/parked-ideas.md`. Of step (a), planned in
`docs/ideas/looks-and-history-plan.md`, one step remains, after local
adjustments (user, 2026-10-08; `docs/ideas/local-adjustment-plan.md`):

- **Step 4, presets (plan §3).** A `PresetStore` in core over
  `AppDataLocation/presets` (the settings document plus a name; a new ADR);
  a Presets list above History; the CLI's `preset list | show | apply` and
  `export --preset`; Python's `presets()`, `load_preset`, `save_preset` and
  `with_look`. Copy/paste itself is done.

- **Presets (step 4 above) must carry the negative (plan step 6).** The preset
  file's `"negative"` object, the carried sections being those whose keys are
  present, and the save dialog pre-checking Demosaic only when it is not AHD.
  `preset apply` writes the negative to the sidecar and decodes nothing.

## GUI tasks (user, 2026-10-09)

`old` is the reference (`src/MainWindow.cpp`, `src/ChromeHider.*`, ADR 0028 there).

- **Spots and Info tabs** in the develop dock, as `old` had (Q switched to
  Spots there).

Done (user, 2026-10-20): **export presets** (branch `export-presets`, ADR 054, plan
`docs/ideas/export-presets-plan.md`, steps 1 to 9; not pushed):
- `ExportSettings` in `include/`, Qt-free; `export-presets.json` in the config directory
  (`ARRAW_CONFIG_DIR`, else `QStandardPaths::GenericConfigLocation/arraw`), versioned as ADR 051,
  with a locked, atomic store; four read-only built-ins (Web 2048 px, Social 1080 px, Full size
  JPEG, Print TIFF 16-bit);
- CLI `export --export-preset`, `--config-dir`, `export-preset list|show|path`; Python
  `ExportPresetStore` and friends; the dialog's picker (Save As, Update, Rename, Delete, Set
  Aside) and Export with Preset in the File and film-strip menus;
- review `docs/reviews/claude-opus-5-5_2026-10-20_export-presets.md` (untracked): mediums
  fixed. Python's `save(settings=)` is removed (user, 2026-10-20: a preset
  is render plus write, and `save` only writes; the recipe is `develop(size=settings.develop_size)`
  then `save`); whether Python wants an `export(photo, path, preset)` is for the Python API
  design pass, not now. Open: a cross-process lock test; `path.string()` on Windows unchecked;
  `--no-profile` and `--resize-filter` are not in presets; develop presets are separate (step 4
  above).

- **Logging throughout the app, with a level set in the app** (user, 2026-10-20). The debug
  window (`src/app/ui/DebugWindow.*`, `QtMessageCapture`) captures what Qt logs, but little
  is logged: the code has three `qDebug/qInfo/qWarning` calls in all and one category
  (`arraw.timing`, `src/trace/TimingTrace.cpp`, warning level), so the window shows a few
  warnings only. Wanted:
  - a **level** (debug / info / warning / critical) **chosen in the app**, remembered in
    `QSettings`, applied at run time (`QLoggingCategory::setFilterRules`) and shown in the
    window; the window also filters by level and category;
  - **logging that permeates the app**: categories per area (`arraw.decode`, `.render`,
    `.gpu`, `.export`, `.filmstrip`, `.sidecar`, `.lens`, `.presets`, ...) with `qCDebug`,
    `qCInfo`, `qCWarning` at the decisions and failures, not only in the app: core is Qt
    Core-linked already, but a library should not depend on Qt logging for the CLI and Python,
    so decide a small sink (a callback or `Diagnostics.h`'s existing path?) that the app
    routes into the same window. To scope: ADR (levels, categories, a sink in core,
    cost when off, thread safety, no personal data such as full paths at info level), the
    `ARRAW_LOG` / `QT_LOGGING_RULES` override, a log file option.

- **Film strip context menu, more operations** (user, 2026-10-20). Now: Rate, Label, Show in
  File Manager, Export…, Refresh Folder. Wanted: **Copy Settings… / Paste Settings** (one
  shot or the selection; paste is one history step per shot, written to each sidecar through
  the saved state, as the multi-shot export does), **Delete** (to the trash, never a hard
  delete; confirm; raw and its sidecar together) and **Delete Sidecar** (reset a shot's edits;
  confirm; only where a sidecar exists). Ideas, not decided:
  - Reset Settings (as Delete Sidecar but keeps marks), Rotate left/right, Copy Path / Copy
    File Name, Open With… / Open in Editor, Reveal Sidecar;
  - Sync Settings from the open shot to the selection (copy + paste in one), Auto-Apply last
    paste;
  - Select by label/rating, Select Edited (has a sidecar), Invert Selection;
  - Apply Preset ▸ (with the develop presets, step 4), Export with ▸ preset (export presets);
  - Rename, Move/Copy To… (with the sidecar following); undo for these file operations is
    the system trash only, so say so in the confirm.
  Items act on the selection if the clicked shot is in it, else on that shot alone; a shot
  with unsaved edits open needs a rule (the saved state, as export does).

Done (user, 2026-10-20): **export from the film strip's context menu.** Export… is offered for
one shot or the selection; `MainWindow::exportShots` asks the settings once, then one folder
(starting at `export/lastFolder`, which the single-photo save dialog also uses), names the files
with `targetPaths` (`_2`, `_3` for equal stems), asks once about files that exist (Replace all /
Skip existing / Cancel) and queues one `ExportJob` per file with `fromSaved`: the export thread
opens the photograph and uses its sidecar's state, so nothing is read on the interface thread.
The jobs report as one batch ("Exported N of M files", failures in the tooltip); the saved state is
used for the open photograph too, and the summary says when its unsaved edits are left out.

Done (ADR 042): the render pie is always present at the left of the status
bar, in the new `StatusLine` widget that also owns the message, device label
and zoom button.

Done (ADR 047): F7 History, F8 Develop, F9 film strip, F11 full screen and
F12 Hide Panels, in the View menu and on the window.

Done (ADR 048): the develop dock has Adjustments and Masks tabs, the tab
following the mask mode; the Adjustments groups collapse, each remembered in
`QSettings`.

## Local adjustments (user, 2026-10-08; `docs/ideas/local-adjustment-plan.md`)

Done: steps 0 to 3 (ADR 044), **step 4, the brush prototype** and **step 5**
(2026-10-10) and **step 6** (done 2026-10-16).
The report `docs/ideas/brush-prototype-report.md` is accepted and ADR 044
sections 6, 7 and 9 are amended. The prototype code
(`src/core/{BrushStrokes,BrushRaster,BrushCoverageCache,StrokeCodec}`) is
on branch `brush-prototype`.

- **Step 5, the brush end to end without GUI: done (2026-10-10).** Model and
  edit rules, the rasteriser and cache in the render path (coverage retained in
  the ladder, a coarser first render at level 0), GPU textures, persistence
  (readable strokes in state JSON, base64 in XMP), `info`, the GUI minimum (list,
  history, tint) and Python (`add_brush_mask`, `with_brush_stroke`). See ADR 044
  "What step 5 settled".
- **Step 6, the brush GUI: done (2026-10-16); Q6 (b) 2026-10-11.** The tool, settings and keys,
  the tail method in the cache, paced requests, the tint, and the measured long
  session (ADR 044 notes "as built" and "Q6 (b)"). While a stroke is under way the
  picture is rendered at most 0.4 MP (a coarser level) and the full one follows the
  release: fit latency 111 ms (was 291), 13 pictures a second, at ratio 2 110 ms
  (was 1322). Not reduced at level 0 (1:1 and beyond), which stays as slow as before
  (latency 4.2 s, commit 3.0 s). At ratio 2 the commit no longer redraws brushes (0
  misses, was 376; superseded Extended parents are evicted first, ADR 044 note
  2026-10-16): p95 2.3 s down to 0.53 s, median unchanged at about 0.5 s.
  Accepted (user, 2026-10-16): the 0.5 s left is rendering the 6 MP level itself;
  the cache has under 4 MiB to spare at ratio 2. The GPU run is not measured (no hardware GPU here).

## Lightroom XMP import (user, 2026-10-20; later, when the develop set is mature)

Not planned. An explicit import of a Lightroom (Camera Raw, `crs:`) XMP sidecar into the
matching arraw settings, never automatic. To scope in an ADR before any code:
- **Mapping table:** `crs:` keys (Exposure2012, Contrast2012, Highlights2012, Shadows2012,
  Whites2012, Blacks2012, Clarity2012, Dehaze, Vibrance, Saturation, Temperature, Tint, tone
  curve points, HSL, colour grading, crop, grain, vignette, lens profile, local adjustments)
  to the descriptor table; different scale and different maths mean an approximation, never
  an identical look. Report what mapped, what was approximated and what was dropped (notices).
- **Where it enters:** a `Sidecar`-adjacent reader in core (so the CLI and Python can use it),
  an app action (File or Edit menu, and the film strip's menu for a selection), and one
  history step per shot with its own origin; the arraw sidecar's namespace is untouched, and
  the foreign XMP is read, never written.
- **Versions:** which Camera Raw process versions (2012, 2010, 2003) to accept; refuse the rest
  with a clear message.
- **Not part of it:** the negative's demosaic and lens corrections have no `crs:` twin; the
  lens profile can only be matched through lensfun by name.

## Snapshots, along the history (user, 2026-10-20 and 2026-10-23; accepted)

Named whole develop states of one image, for A/B comparison (`docs/desired-features.md`,
`docs/ideas/architecture-roadmap.md` "Room for Snapshots", `looks-and-history-plan.md`).
The accepted architecture is a tree of immutable, full-state snapshots. See
[ADR 056](adr/056-snapshots-are-per-photo-looks-with-independent-history-and-sidecar-storage.md)
and the [architecture proposal](ideas/snapshots-architecture-proposal.md). Implementation is
tracked as five reviewable gates; do not begin a later gate before the previous one is reviewed
and committed:

0. **[x] Architecture.** Record the decisions, initial bounds, verification requirements, and
   implementation sequence (this ADR and proposal).
1. **[x] Model and persistence.** Add snapshot value types and `OpenDocument`; sidecar v2 with
   `activeStatePresent`, JSON snapshot nodes, tolerant reads, strict writes, targeted CRUD, shared
   `QLockFile`/`QSaveFile`, and frozen v2 fixtures. Verify v1/v2 compatibility, RAW defaults,
   foreign XMP preservation, atomicity, revision conflicts, lock timeout, and concurrent writes.
2. **[x] Session history.** Add working/saved base IDs to
   `EditSession` and history; implement Create anchoring, exact Restore steps, rename/delete remapping, and dirty/save/discard/undo/redo
   behavior. Verify that Create preserves redo and Undo never removes a durable snapshot.
3. **[x] Thumbnails.** Render explicit snapshot recipes at low priority, validate/cache bounded
   JPEGs, and patch only the matching node by ID and render key. Verify stale results, size bounds,
   and that completion does not alter active edits or dirty state.
4. **[ ] GUI.** Add one `SnapshotModel` tree above History, preview without session mutation, explicit
   Restore, and create/rename/delete actions. Verify preview versus restore, edit disabling, and
   refresh after external changes. Also a way to discard unreadable snapshot entries: they stay in
   the sidecar and count toward the 32-snapshot limit, and the errors now say so, but nothing
   removes them yet (deferred from gate 1).
5. **[ ] CLI and Python.** Extend `info`, add `export --snapshot ID`, and bind the same document,
   collection, and session operations. Verify text/JSON parity, unknown IDs, option conflicts,
   repeated exports, and Python parity.

Initial bounds (provisional pending measurement): 32 snapshots per photo; nonempty names up to
256 UTF-8 bytes; thumbnails at most 256x256 and 64 KiB JPEG; serialized UTF-8 XMP snapshot
subtree at most 128 MiB, including escaping and base64. IDs/revisions are decimal strings in
JSON. Sidecar writers wait up to 2000 ms for the
shared lock and report failure without an unlocked fallback.

Product behavior:

- **Model:** each snapshot has a stable ID, optional parent ID, name, time, `DevelopState` and
  negative. Its parent records which snapshot the working look was based on when captured;
  differences along an edge are computed for display, not stored as the snapshot. Leave room for
  rebasing or merging later without making either part of the first version.
- **History:** each step carries the working base snapshot ID alongside state and negative.
  Restore adds an undoable step with the restored snapshot as base; later edits keep that base.
  Create Snapshot immediately writes a new child and makes it the base of the current history
  position without adding a step; Undo reaches the preceding edit and its base, never deleting
  an already written snapshot. Ordinary Save writes the active settings and base without making
  a snapshot. Creating one leaves the saved active settings and base untouched, so reopening can
  show an older look/base and the new node.
- **Persistence:** snapshots and the saved working base belong in the XMP sidecar. Raise the
  sidecar to version 2 on every Arraw sidecar write, including marks-only writes. Older builds
  then refuse writes even to photos without snapshots; the user accepts this. Amend ADR 051 and
  add frozen version-2 fixtures. In v2, track whether saved active develop settings are present
  separately from the version key so a marks-only RAW keeps its normal defaults. Measure the size
  of repeated brush states. Embed a small rendered thumbnail per snapshot (at most 256x256); a
  missing or damaged one is regenerated without fuss or changing its settings.
  Deleting a snapshot that was the saved base moves that base to its parent (or no base) without
  changing the saved settings.
- **UI:** a tree above History with thumbnails; clicking previews without changing the session,
  and Restore explicitly switches the working look. Create, rename and delete write the
  snapshot collection immediately. Deleting a snapshot reparents its children to the deleted
  node's parent; if it had none, they become roots. Their full looks stay unchanged and edge
  differences are recomputed. If it was the working base, keep the working settings and use its
  parent as the base, or no base when it had no parent; remap references in session history too.
  Snapshot thumbnail completion patches only its node and never saves pending develop edits. A
  split A/B view can follow.
- **CLI:** `info` lists snapshots, including stable IDs, names and parents, in text and JSON;
  `export --snapshot ID` renders that snapshot's full settings and negative. Repeated exports
  of one input with distinct output paths enable scripted batch exports of its versions. A
  name may be accepted only if it resolves uniquely. A one-command multi-snapshot export is a
  separate design question.
- **Other surfaces:** Python uses the same model; copy/paste does not treat a snapshot as a copy
  section. A film strip badge can follow.
- **Relation to presets:** a snapshot is per image and keeps everything; a preset is shared and
  carries chosen sections. "Save snapshot as preset" is the bridge.

## Merging frames: bracketing, panorama, focus stacking (user, 2026-10-18; speculation, needs scoping)

Not planned. Wanted, not necessarily inside arraw: lean on external OSS tools where
they exist. Scope, ADR and plan are required before any code.

Sketch: arraw renders each selected frame to a linear 16-bit TIFF in a temp directory
(lens stage on or off, to be decided: Hugin does its own geometry), runs an external
command, and imports the result as a new photo.
- **Phase 1, generic "external merge":** N selected images, a configurable command
  template, presets for `focus-stack` (focus stacking), Enfuse with
  `align_image_stack` (bracketing, exposure fusion) and Hugin's `pto_gen`, `cpfind`,
  `autooptimiser`, `nona`, `enblend` (panorama). Headless by default, plus "Open in
  Hugin" (a generated `.pto`) when the result is poor. Hugin has a GUI; the rest do
  not.
- **Phase 2, own HDR merge:** weighted average in linear space divided by exposure
  time; the one case where doing it ourselves pays off (editable scene-referred
  result). Registration is still needed, so own the merge, not the alignment:
  1. tripod-only first (no alignment, linear merge only);
  2. then alignment through `align_image_stack` (optional runtime dependency);
  3. or through OpenCV (`createAlignMTB` for brackets, ECC or features for the
     rest; a new library to build and package on all three platforms).
  Caveats: handheld brackets differ in exposure, so compare by median-threshold
  bitmaps or gradients, not raw pixels; moving subjects need a separate per-pixel
  ghost weight; focus stacks also change scale (focus breathing), so they need a
  similarity or affine transform, and misalignment shows as halos; panorama needs
  features, a camera model, bundle adjustment and seam blending, so leave it to
  Hugin.
- **Alternatives for panorama:** OpenCV's stitching module (embeddable, weaker);
  hand-off only (export, open Hugin).

To scope:
- result format: linear TIFF, float TIFF or linear DNG (needs a DNG writer);
- provenance: record the source frames in the result's sidecar?
- detection or bundling of the tools; Windows and macOS packaging of Hugin is hard;
- CLI and Python exposure (favours the command template);
- lens stage before or after the merge; the bracket's per-frame exposure metadata;
- no focus-stacking GUI exists upstream, so arraw would need its own around
  `focus-stack`.

## Follow-ups (known limits, all documented in ADRs)

- **Dehaze:** convex tips sharper than the window (corners, the ends of
  elongated bright areas) keep up to ~0.65 stop less Dehaze. Fix: an
  edge-aware floor such as a guided filter (ADR 041).
- **Colour noise reduction** is a plain blur and may bleed at saturated edges.
  It now defaults to 25 for RAW files, so judge it on a real high-ISO RAW
  (ADR 039).
- **Crop mode:** Lightroom may stay in the crop tool on the next photo; ours
  commits and leaves it (ADR 040).
- **GPU cancellation** only acts between renders (ADR 042).
- **Python:** no progress callback yet (ADR 042).
- **LibRaw may resize after decoding** (`stretch()` for non-square pixels,
  `fuji_rotate()` for SuperCCD), so the decoded size can differ from
  `ImageMetadata::size`, which the CLI, Python and `Edits.h` plan against;
  unverified, needs a sample file. Options: the post-processing size in the
  metadata, LibRaw's resizing off, or the decoded shape wherever it exists.
- **Responsiveness, not measured:** leaving the crop mode and rotating, the
  crop overlay's pixmap conversion, strip painting with many cells, folders far
  larger than 300 shots or on a network share (ADR 043).
- **Closing during a cold camera-preview read** waits for it (under a second);
  the read cannot be stopped part-way (ADR 043).
- **PNG export** takes ~6 s at 24 MP; a lower zlib level is the lever (ADR 043).
- **Dock width:** `minimumDockWidth` asks the panel's style for the scroll bar
  extent, not the scroll area's; equal under Fusion and Breeze.
- **Small-fixes review leftovers:** `canResumeFrom` plans a render twice on
  every resume (L3); the cache's new error path has no test (L4); Windows code
  (manifest, console, `gpuUsedByDefault`) was never compiled; MinGW would need
  an `.rc` for the manifest; `CLAUDE.md`'s structure list does not name
  `src/trace` or `src/platform` (the user's file).
- **Hardware:** the suite runs on lavapipe. One real GPU (Mesa ANV, Intel HD
  Graphics 630) has been used, on 2026-09-30, to measure the parity tolerances
  (ADR 017, `tests/gpu/GpuTesting.h`). The passes added since (Presence, effects,
  noise reduction), the GUI and performance have not been run on hardware.
