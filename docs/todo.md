# To-do

As of 2026-10-07, branch `copy-settings`, nothing pushed. Earlier, on
`finish-global-adjustments`: the global-adjustments plan, phases 1–7
(`docs/ideas/global-adjustments-plan.md`), the crop and rotation tool
(`docs/ideas/crop-ui-plan.md`), and render progress with cancellation
(`docs/ideas/render-progress-plan.md`). The reviews are in `docs/reviews/`,
untracked on purpose.

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
`docs/ideas/looks-and-history-plan.md`, two steps remain:

- **Step 3, session history (plan §4).** `EditSession` keeps a list and a
  position; `goTo`, origins (Edit, Paste, Preset, Reset, Crop) and
  `describeChange` (ADR 022 amended); a `HistoryModel` and a History dock on
  the left, where a click navigates; Python binds `EditSession`, with
  `with session.edit():`.
- **Step 4, presets (plan §3).** A `PresetStore` in core over
  `AppDataLocation/presets` (the settings document plus a name; a new ADR);
  a Presets list above History; the CLI's `preset list | show | apply` and
  `export --preset`; Python's `presets()`, `load_preset`, `save_preset` and
  `with_look`. Copy/paste itself is done.

- **Demosaic and lens corrections as an import group (user, 2026-10-07).**
  - **Today:** neither exists on this branch. `DecodeOptions` has only
    `halfSize`, and `SettingGroup` has no group for what shapes the decoded
    negative.
  - **The aim:** the demosaic choice and lens corrections become a group of
    their own, *Import*, or sit even higher in the model: settings of the
    negative, applied before every develop stage.
    - Changing one re-decodes or re-corrects the source and invalidates every
      checkpoint.
    - No develop edit ever does that.
  - **What `main` did:**
    - ADR 0036: a demosaic choice that re-decodes through the load path, over
      LibRaw's LGPL built-ins only (AHD, VNG, PPG, DCB, DHT, AAHD, Linear),
      saved as a stable string token.
    - ADR 0032: lens corrections (distortion, vignetting, lateral CA) as on/off
      switches driven by a lens profile, from lensfun or the file's own profile.
      They are applied once on the CPU to make a corrected negative beneath
      spots, masks and the shaders.
  - **Decide:** whether these belong in `DevelopState` as a group, or with the
    `Photo` as part of its negative (beside the decoded pixels and the
    pyramid). Also how presets and copy/paste treat them (a lens profile is
    per shot), and where this fits in the roadmap's stage table, step (c).

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
