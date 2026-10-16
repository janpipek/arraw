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

Status (2026-10-16), to pick up later:
- uncommitted before this note; committed on `negative-settings`, not pushed,
  not merged; ADR 049 still says "proposed";
- a fresh Opus pass should confirm the fixes, then mark ADR 049 accepted;
- review findings 5 to 8 (lower) are open; no tests yet for `goTo`, discard,
  a sidecar reloaded after changing on disk, nor an export during a re-decode
  at window level (queue level only);
- `docs/photo-development.pdf` is stale against section 3.4 (no PDF recipe);
- `PresetStore` (looks plan step 4) must carry the negative (plan step 6);
- Phase 2, lens corrections (ADR 050): embedded profile first, then lensfun;
- known flaky under load: the brush repaint app tests, "An observed loop
  covers every row once".

## Done: session history (branch `copy-settings`, 2026-10-08)

Step 3 of `docs/ideas/looks-and-history-plan.md`: `EditSession` keeps a list
and a position, `goTo`, origins and `describeChange` (ADR 022 amended); a
`HistoryModel` and a History dock on the left, where a click navigates;
Python binds `EditSession` with `with session.edit():`. Reviewed in
`docs/reviews/claude-opus-5-5_2026-10-08_session-history.md`. Low leftovers:
- `groupDisplayName` belongs in `SettingPresentation`, not `HistoryModel.h`
  (the panel includes the model's header for it) (N1);
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

- **Lens corrections as settings of the negative (Phase 2, ADR 050 to write).**
  Distortion, vignetting and lateral CA as switches in `NegativeSettings::lens`,
  applied once on the CPU beneath spots, masks and the shaders; the profile
  comes from the file or lensfun (`auto | embedded | lensfun`). The demosaic
  half is done (above). ADR 049 section 7 and the plan's "Phase 2 sketch" list
  what ADR 050 must settle: packaging, the database version, stage placement,
  and the frame crop and mask coordinates live in.
- **Presets (step 4 above) must carry the negative (plan step 6).** The preset
  file's `"negative"` object, the carried sections being those whose keys are
  present, and the save dialog pre-checking Demosaic only when it is not AHD.
  `preset apply` writes the negative to the sidecar and decodes nothing.

## GUI tasks (user, 2026-10-09)

`old` is the reference (`src/MainWindow.cpp`, `src/ChromeHider.*`, ADR 0028 there).

- **Spots and Info tabs** in the develop dock, as `old` had (Q switched to
  Spots there).

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
