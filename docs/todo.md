# To-do

As of 2026-10-07, branch `finish-global-adjustments`, nothing pushed.
Done on this branch: the global-adjustments plan, phases 1–7
(`docs/ideas/global-adjustments-plan.md`), the crop and rotation tool
(`docs/ideas/crop-ui-plan.md`), and render progress with cancellation
(`docs/ideas/render-progress-plan.md`). The reviews are in `docs/reviews/`,
untracked on purpose.

## Done: responsiveness

Opening decodes off the GUI thread (ADR 043), the resize, the geometry, the
zero fill of `ImageBuffer` and export sharpening are banded, the progress
weights were remeasured, and the develop dock keeps every slider at least
120 px wide. Reviewed in
`docs/reviews/claude-opus-5-5_2026-10-06_responsiveness.md`, fixes applied.

## Next

The order and the decisions are in `docs/ideas/architecture-roadmap.md`;
loose ideas are in `docs/ideas/parked-ideas.md`. Step 0 (the review's small
fixes and the Windows CPU default) is done. Step (a) is planned in
`docs/ideas/looks-and-history-plan.md` (accepted 2026-10-07); next is its step
1, the geometry rules into core.

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
