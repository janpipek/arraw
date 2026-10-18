# Lens corrections are a stage between noise reduction and the chain

Status: accepted, 2026-10-18 (proposed 2026-10-17). The plan is
[lens-corrections-plan.md](../ideas/lens-corrections-plan.md). It is Phase 2 of
[ADR 049](049-settings-of-the-negative-sit-beside-the-develop-state.md).

A photographer wants profile-driven correction of distortion, vignetting and
lateral chromatic aberration (`desired-features.md`, "Lens Corrections"). ADR
049 placed the switches in `NegativeSettings` and sketched the rest. These
decisions are made by the user:

- **Corrections are off by default**, for new photographs too (2026-10-17).
- **A cached `Stage::Lens` after noise reduction** (option c below, 2026-10-17).
- **The automatic lens match is recorded in the sidecar**, so a lensfun
  database update does not change which lens an edit uses (2026-10-17).
- **lensfun is the only source in v1, matched from the EXIF as `old` did**,
  with `old`'s faults fixed (2026-10-17). This replaces the 2026-10-16 decision
  "embedded, then lensfun". Embedded profiles become a later, optional step.
- **lensfun is managed as `old` managed it**: a tri-state CMake option, required
  in Linux release builds, `AUTO` in development, optional and deferred on
  Windows and macOS (2026-10-17).
- **RAW only** in v1 (2026-10-17).
- Lens settings travel with presets and copy/paste as `CopySection::Lens`
  (2026-10-16).
- [ADR 009](009-reference-frames.md) fixes the frames. Spots and brush strokes
  live in the sensor frame. Linear and radial masks and perspective guides live
  in the corrected frame. The crop lives in the upright frame, after lens
  correction.

The user's camera is a **Canon EOS 80D** (CR2, crop factor 1.6). It is the
reference body for v1.

The branch `origin/old` (ADR 049 calls it `main`) had this feature as its ADR
0032. The plan evaluates it. In short:

- **Kept:** three switches without sliders; one resolved radial model that the
  source fills; vignetting gain, distortion and lateral CA in one resample; the
  frame auto-filled; lensfun's `lfModifier` resolving its own models; the
  dependency management (CMake tri-state, packaging, database staging and
  lookup).
- **Not kept:** where it ran. A session object corrected the preview and the
  full-size buffers itself, which is why batch export skipped correction
  (`reimplementation-plan.md`).
- **Fixed:** matching took the first camera and the first lens, with no score
  and no mount check when the camera was not found; the database was loaded on
  every open; the lensfun version was never checked; resampling was bilinear;
  the fill zoom depended on the resolution; the table was sampled from the
  geometric centre, not lensfun's; a missing profile was a silent no-op.

What exists today (verified, 2026-10-17):

- **The decode.** `rawimport::load` gives `RgbaU16` in sensor geometry
  (`user_flip = 0`) with clipped highlights. lensfun's profiles are calibrated
  in that geometry.
- **The stage table.** Denoise, Pointwise, Geometry, Resize, Effects
  (`src/core/StageTable.h`). `ProcessingPlan::decode` is a header outside
  `stagesOf`.
- **The geometry pass** is bilinear (ADR 014).
- **The brush.** `sensorFrom(CorrectedPosition)` is the identity, waiting for
  this ADR.
- **The render API** is `(source, DevelopState, request)` throughout. Nothing
  in it can reach `Photo::negative()`.
- **What a CR2 says about its lens.** Exiv2 0.28.8 (checked in its tag table
  here, not yet against an 80D file) exposes `Exif.Photo.LensModel`,
  `Exif.Canon.LensModel`, `Exif.CanonCs.LensType` (a numeric ID that Exiv2
  names), `Exif.CanonCs.ShortFocal`/`Lens`/`FocalUnits`,
  `Exif.CanonCs.MaxAperture` and `Exif.CanonFi.FocusDistanceUpper`/`Lower`.
  LibRaw 0.21.5 gives `lens.makernotes.LensID`, `MinFocal`, `MaxFocal`,
  `MaxAp4CurFocal` and `FocalLengthIn35mmFormat`.
- **What a CR2 says about corrections.** Exiv2 decodes only the camera's
  *switches*: `Exif.CanonVigCor2.PeripheralLightingSetting`,
  `ChromaticAberrationSetting` and `DistortionCorrectionSetting`, and
  `Exif.CanonLiOp.PeripheralIlluminationCorr` and `DigitalLensOptimizer`.
  `Exif.Canon.VignettingCorr` (0x4015) is an undecoded array. LibRaw has no
  correction data for Canon. **As far as we know, a CR2 carries no usable
  distortion or vignetting profile**: the coefficients live in the camera's
  and DPP's lens data, not in the file. This is to be confirmed on a real 80D
  file (`exiftool -v3`).
- **lensfun** is not installed in the sandbox (`pkg-config lensfun` fails on
  Ubuntu 26.04). No file of this repo names it.

## Decision

### 1. The settings: three switches and a recorded lens

`NegativeSettings` gains `lens`:

```
LensSettings  distortion, vignetting, lateralCa: bool   (default false)
              lens: optional<RecordedLens>
RecordedLens  maker, model: string     (lensfun's names, exact)
              cropFactor: double       (the image's, as the match used it)
              origin: LensOrigin { Matched, Chosen }
              database: string         (the database stamp at recording)
```

An absent key means off and no recorded lens, so every existing sidecar
renders as it does today. The default for new photographs is off. The settings
apply to a RAW only. For a JPEG or a TIFF they are not applicable and are
skipped, as White Balance and Demosaic are.

**No source field in v1.** With lensfun the only source, an
`Auto/Embedded/Lensfun` field would be a combo with one working choice, its
tokens, a sidecar key and tests, for nothing. The migration path is in §8.

### 2. A stage of its own, after Denoise, before the chain (option c)

`Stage::Lens` sits between `Denoise` and `Pointwise`, with
`ProgressStep::Lens`, a table row, a plan block `ProcessingPlan::lens` (a
`LensPlan` in `stagesOf`) and a rung. It runs only when at least one resolved
correction is on. Otherwise it collapses onto Denoise, so render digests do not
change by default. In one pass, for each output pixel `c`:

1. map `c` to its sensor position (centre, fill scale, then the distortion
   table);
2. for lateral CA, offset red and blue from that position by their own tables,
   evaluated at the sensor radius;
3. sample each channel there;
4. multiply by the vignetting gain at the sensor radius.

The output is `RgbaF32` in the source's encoding, with orientation still
pending, so a gain above 1 does not clip. It is ADR 009's **corrected frame**.

Why this placement:

- **Noise reduction keeps seeing unresampled noise** (ADR 039).
- **Nothing else changes frame.** The chain, its masks, the geometry and crop
  fitting already run in the corrected frame.
- **It is cached.** A develop edit resumes from the Lens rung. A lens toggle
  resumes from the Denoise rung and decodes nothing.
- **One place does it.** Preview levels (ADR 023), the half-size thumbnail,
  export, the CLI and Python all get it through the plan. No caller can skip
  it, which was `old`'s defect.

The cost: another row, so `Stage` values after Denoise shift (Python binds them
by name), a float rung of source size, and a second resample when straightening
is also on.

### 3. One model, in normalised coordinates

lensfun resolves into `LensCorrection` (public, `include/LensCorrection.h`):

- per-correction tables over the normalised radius: distortion as a radial
  scale, lateral CA as red and blue scales against green, vignetting as a gain;
- the optical centre, **lensfun's calibrated one**, which the tables are
  sampled from;
- a provenance per correction (lens identity, crop factor, focal length,
  aperture, distance and whether each was read or assumed, database stamp).

The model is source-independent, so a later embedded reader fills the same
struct. The radius is normalised to the distance from the centre to the
farthest corner, and coordinates to the buffer's own size, so a pyramid level,
a half-size decode and the full decode resolve to the same tables (ADR 009).
The table size is chosen by measurement: under 0.05 sensor pixels at the
corner against `lfModifier`'s direct answer.

**The fill scale** is the smallest zoom ≥ 1 that keeps every output sample
inside the source. It is computed once from the distortion table in normalised
coordinates and stored in the plan, so every resolution agrees. The corrected
frame keeps the source's size and aspect, so ADR 014's rectangular valid
region and crop fitting do not change. There is no "fill" switch in v1.

**Resampling** is bicubic Catmull-Rom (4×4 taps), not `old`'s bilinear, which
softens every corrected photograph at half-pixel offsets. Lanczos-3 is the
alternative if measurement says its 2.25× cost is worth it. Vignetting alone is
a gain and does not resample. Lateral CA alone copies green exactly.

### 4. The render takes the correction explicitly

The public render functions (`develop`, `developUntil`, `resumeFrom`,
`canResumeFrom`, `resumeOrDevelop`, `drawsBrushCoverage`, `sample`, the GPU's
counterparts, `planFor(buffer, …)` and `PreviewRenderer::request`) gain a
`const LensCorrection&` after the state, **with no default**. Whoever holds a
`Photo` passes `lensCorrectionFor(photo, database)`, the rendering twin of
`decodeOptionsFor`. Tests and pixel-only callers pass `LensCorrection{}`.
`planFor(photo, …)` resolves it itself. ADR 049's rule holds: nothing that
takes only a `DevelopState` can change the negative.

### 5. lensfun, matched from the EXIF, fixed

- **Facts, read once.** `ImageMetadata` gains `lens` (`LensFacts`), filled when
  a RAW's metadata is read: camera maker and model (EXIF, then LibRaw's
  normalised ones); the lens name candidates (below); the lens's focal range
  and maximum aperture from the maker note; the focal length, the aperture, the
  focus distance when known, and the 35 mm focal length.
- **The database, loaded once.** One process-wide `LensDatabase`, loaded off
  the GUI thread, read-only afterwards, lookups behind a mutex. The directory
  is found as `old`'s `bundledLensfunDbDir` found it, with std types instead of
  Qt:
  1. an explicit directory (`fromDirectory`, the CLI's `--lens-database`,
     tests);
  2. a database bundled beside the executable: `<exe>/../share/lensfun/db`
     (AppImage) or `<exe>/lensfun/db` (Windows, portable);
  3. lensfun's own `Load()`: the system directory and the user's
     `lensfun-update-data` copy, which lensfun prefers when newer.

  A bundled copy is also replaced by the user's update when that is newer,
  which `old` did not do. `stamp()` is the newest database timestamp.
- **Matching** (`old` took `cams[0]` and `lenses[0]`):
  - the camera is matched first. Its mount and crop factor constrain the
    lenses. No camera means the crop factor comes from `focalLengthIn35mm /
    focal`, and lenses are then searched without a mount, which is allowed
    only when exactly one lens passes the other filters;
  - each lens name candidate is tried in order, with lensfun's fuzzy score;
  - candidates outside the file's focal range or maximum aperture are
    dropped;
  - a score below a threshold is no match;
  - **two distinct lenses within a margin give `LensAmbiguous`, and nothing
    is applied.** A wrong lens is worse than none. The status lists the two,
    and "Choose lens…" resolves it.
- **Canon names, the reference case.** The candidates for a CR2, in order:
  1. `Exif.Photo.LensModel`, which Canon writes without the maker
     (`EF-S18-135mm f/3.5-5.6 IS USM`; lensfun lists `Canon EF-S 18-135mm
     f/3.5-5.6 IS USM`, and its fuzzy matcher is expected to join them);
  2. `Exif.Canon.LensModel`;
  3. Exiv2's name for `Exif.CanonCs.LensType`, which Exiv2 narrows by the
     focal range and aperture.

  **Sigma and Tamron** report a Canon lens ID, often one that Canon or another
  third party also uses, so the ID's name can be wrong and `LensModel` can be
  a Canon name, a bare focal range or empty. Hence the focal-range and
  aperture filter, the ambiguity rule and the manual choice. The lens maker is
  never used as a filter, because Canon bodies do not reliably write
  `LensMake`. Which strings a real 80D writes for each lens is unverified; the
  plan pins it on the user's files.
- **Sampling.** `lfModifier` per correction, in lensfun's frame with its
  calibrated centre and crop rescaling, sampled into the tables. lensfun's
  models are not reimplemented. An unknown distance takes lensfun's default
  and the provenance says "assumed"; `old` assumed f/8 and 1000 m silently.
- **Version.** `src/core/LensfunSource.cpp` is the only file that includes
  `<lensfun.h>`. It targets the 0.3.x API and the version 1 database, checks
  `LF_VERSION` at compile time, and keeps a branch for 0.3.95 (version 2) if a
  supported platform ships it.

### 6. The recorded match and the notices

`resolveLens(settings, facts, database)` gives the `LensCorrection` and its
notices.

- **A recorded lens is looked up by its exact lensfun name** and used with its
  recorded crop factor. No fuzzy matching runs, so a database update cannot
  move the edit to another lens. If the recorded lens is no longer in the
  database, the corrections are skipped with `LensProfileMissing`. They are not
  silently re-matched. A database whose stamp differs from the recorded one
  gives an informational `LensDatabaseChanged`, because the same lens's
  calibration may have been revised.
- **No recorded lens**: the facts are matched (§5).
- **Recording.** When a step turns a correction on for a photograph with no
  recorded lens, the same step records the match (`origin = matched`). The app
  does it in `setNegative`, and paste and preset application do it for each
  target. `recordLensMatch(const Photo&, const LensDatabase&)` is the one core
  function, public for Python. The app's lens switches are disabled until the
  database has loaded, so a toggle never waits on it. Undo removes the toggle
  and the record together. Turning the switches off keeps the record.
- **"Choose lens…"** writes `origin = chosen`. "Reset to automatic" clears the
  record and matches again.
- **When nothing is found**, the switches stay as set: they persist, copy and
  undo. Each correction that is on but unresolved is skipped, and one
  per-photograph notice names the reason, which `old` never did:
  `LensUnidentified`, `LensFocalUnknown` (a zoom with no focal length),
  `CameraNotInDatabase` (informational, matching continues),
  `LensNotInDatabase`, `LensAmbiguous`, `LensProfileMissing`,
  `LensDatabaseMissing`, `LensfunUnavailable` (built without it).

  An export goes ahead. The app's Lens rows say "No profile: …" and offer
  "Choose lens…". The CLI's `info` prints the same.

### 7. Frames: ADR 009 stands, and the brush is mapped forward

- **Crop**: in the upright frame, after the Lens stage. Its normalised
  rectangle keeps its meaning. Toggling distortion moves the content beneath
  it radially about the optical centre, scaled by the fill. Lens corrections
  never write the crop.
- **Linear and radial masks**: in the corrected frame, unchanged. They stay
  straight lines and ellipses and move against the subject on a toggle, the
  trade ADR 009 accepted.
- **Brush strokes**: stored in the sensor frame. The coverage is drawn in the
  corrected frame by mapping each dab centre forward (a Newton inverse of the
  radial table), radii unchanged. The `BrushCoverageCache` key gains the lens
  block. The pointer path becomes `sensorFrom(lens, corrected)`.
- **Spots, later**: in the sensor frame, as a stage before Lens.

### 8. Identity, persistence and copying

- **Sidecar** (`arraw:`, no version bump, written by ADR 049's "differs from
  what reading gave" rule): `lensDistortion`, `lensVignetting`,
  `lensLateralCa` (`True`/`False`); `lensMaker`, `lensModel`,
  `lensCropFactor`, `lensOrigin` (`matched`/`chosen`) and `lensDatabase`.
  Coefficients are not written.
- **Migration to embedded sources.** When an embedded reader lands, it adds
  `arraw:lensSource`. An absent `lensSource` with a recorded lens means
  `lensfun`, so every v1 edit that corrected anything keeps rendering from
  lensfun. An absent key without a recorded lens means the default of that
  time.
- **JSON**: `negativeToJson` gains a `"lens"` object.
- **Plan and caches**: the resolved tables live in `LensPlan`, **not in the
  decode block** (this amends ADR 049 §7). A database change makes the Lens
  rung stale by value, and the Denoise rung survives. The thumbnail key gains
  a digest of the resolved correction.
- **Copying**: `CopySection::Lens` (`"lens"`) carries the three switches. It is
  ticked by default, with the same one-time migration of remembered selections
  as Demosaic. The recorded lens is per shot and is never copied; the target
  records its own match.

### 9. GPU

The GPU implements `Stage::Lens` as one fragment pass over the same tables,
held to the CPU by a per-stage comparison test. Until then, `GpuDevelop`
refuses a plan whose lens block is active and says why; the callers already
fall back and say so (ADR 015, ADR 017).

### 10. lensfun is a managed optional dependency, required for release on Linux

lensfun is LGPL-3 and its database CC BY-SA 3.0, both compatible with GPL-3.
Its management is `old`'s (verified read-only on `origin/old`), mapped onto
this repo:

- **CMake.** `ARRAW_WITH_LENSFUN` is a cache string: `AUTO` (default: built
  when found), `ON` (required: configure fails if missing), `OFF` (compiled
  out). pkg-config first, then `find_library`/`find_path` with lensfun's glib
  dependencies, as `old`'s `CMakeLists.txt:129-177`. Linked **PRIVATE** to
  `arraw` with a private `ARRAW_HAS_LENSFUN` (`old` linked it PUBLIC). The
  public API reports it at run time (`lensfunAvailable()`), so no lensfun type
  or macro reaches `include/`.
- **Linux release builds use `ON`**: the Fedora spec, `just release` on Linux
  (and so `just deb`), and CI. Development builds use `AUTO`.
- **Fedora**: `BuildRequires: pkgconfig(lensfun)`, `Requires: lensfun` (the
  database is data no soname dependency finds) and `-DARRAW_WITH_LENSFUN=ON`.
- **Ubuntu/Debian**: `liblensfun-dev` and `liblensfun-data-v1` to build;
  the `.deb` depends on `liblensfun-data-v1` explicitly.
- **AppImage** (none in this repo yet): stage `/usr/share/lensfun/version_1`
  into `AppDir/usr/share/lensfun/db` and check in CI that the library and at
  least one XML are inside, as `old`'s `build-appimage.sh` and `release.yml`
  did.
- **Windows** (deferred): vcpkg `lensfun`, and a script copying the database
  beside the executable, as `old`'s `tools/dev_lensfun_db.py`.
  **macOS** (deferred): Homebrew `lensfun`.

A build without lensfun is honest about it, which answers the first draft's
objection to an optional dependency: each photograph with a correction on gets
`LensfunUnavailable`, and `--version`, `info` and the About dialog say
"lensfun: not built". The About dialog and `info` name the database stamp and
carry the database's attribution.

## Consequences

- **Cost of a change.** A lens toggle costs one Lens pass of the shown pyramid
  level plus the chain, and never decodes; ADR 049's re-decode trigger compares
  `DecodeOptions` and ignores it. A Denoise edit re-runs the Lens stage when
  corrections are on. Full-size export pays one float pass of source size.
- **Memory.** One more float rung of source size when level 0 is laddered:
  about 384 MB at 24 MP (the 80D's size).
- **Interface changes.** `Stage` and `ProgressStep` gain a value, and every
  render signature gains the correction. CPU and GPU change together.
- **Reproducibility.** A recorded lens keeps the match fixed across database
  updates. The same lens's calibration can still be revised by an update;
  `LensDatabaseChanged` says so. A development build without lensfun renders
  corrections as off, and says so.
- **Documents edit themselves once.** Turning a correction on writes the
  matched lens into the negative. This is deliberate: it is what makes the edit
  reproducible.
- **Amendments.** ADR 009 gains a note (the frames realised, the forward-mapped
  brush, the fill scale). ADR 012 and ADR 045 gain the Lens stage and block.
  ADR 044's `sensorFrom` is no longer the identity. ADR 049 §7 changes: lensfun
  only in v1, the sidecar records the match, and the profile goes into the Lens
  block, not the decode block. ADR 031: thumbnails read the negative from the
  sidecar.
- **Naming.** The creative "Vignette" (ADR 037, `CopySection::Vignette`) and
  corrective "Vignetting" sit side by side. The UI labels the effect "Post-crop
  vignette". Stored names do not change.

## What was rejected, and why

- **(a) The corrected negative made in the decode**, `old`'s design and ADR
  049's literal wording. Every toggle becomes a 1 to 3 s re-decode (or a second
  source cache), noise reduction sees resampled noise, the source becomes float,
  and the brush still needs the forward map.
- **(b) Vignetting and lateral CA early, distortion folded into the Geometry
  warp.** Fewest resamples for green, but the chain would run in the sensor
  frame (every mask needs a per-pixel map), lateral CA must precede the camera
  matrix, and crop fitting would face a curved region. Kept as the upgrade path
  if the double resample measures badly.
- **Embedded profiles in v1** (DNG opcodes, Sony, Fujifilm). The user's body
  is Canon, and a CR2 carries none that we can read. They are a later optional
  step (plan, "Later").
- **An `Auto/Embedded/Lensfun` source field now.** It would offer one working
  choice. The recorded lens gives the migration (§8).
- **Not recording the match** (the first draft). A database update would move
  old edits to another lens without a word.
- **Recording the coefficients** in the sidecar (not chosen yet; the plan asks
  the user). Pixels would be reproducible too, but a sidecar would hold derived
  data that the database owns, and a pinned calibration would never improve.
- **Applying the best match on a near-tie.** A wrong lens is worse than none.
- **lensfun required everywhere** (the first draft). It blocks Windows and
  macOS builds for a feature the user does not need there yet.
- **The correction carried on the source buffer**, like `decodedWith`. A
  buffer cannot be relabelled without a copy, and a new source clears the
  Denoise rung.
- **Lens switches in `DevelopState`.** Ruled out by ADR 049.
- **Reimplementing lensfun** (its XML and models). It would own matching,
  crop rescaling, interpolation and an XML parser in core.
- **Adobe's `crs:LensProfile*` keys**, which `old` claimed. They name Adobe
  profiles; a round-trip cannot hold.

## Amendment, 2026-10-18: as built

Steps 0 to 10 of the plan are built. Where the code differs from the text above,
the code holds:

- **Failures are a `LensProblem`, not a `Notice`.** `LensProblem`
  (`include/LensProblem.h`) names why a correction was not resolved
  (`LensUnidentified`, `LensFocalUnknown`, `CameraNotInDatabase`,
  `LensNotInDatabase`, `LensAmbiguous`, `LensProfileMissing`,
  `LensDatabaseChanged`, `LensDatabaseMissing`, `LensfunUnavailable`). The
  resolution carries it, and a render turns it into the matching diagnostic.
  The diagnostics also gain `LensNotCalibrated`: the lens was found but has no
  calibration for a correction that is on, so that one is skipped.
- **`sampleLensfun` takes the database** (`LensDatabase`) and the match; the
  only file that includes `<lensfun.h>` is still `LensfunSource.cpp`.
- **The tables have 129 nodes** (`lensTableNodes`), chosen by measurement: linear
  interpolation stays under 0.05 sensor pixels at the corners and edges of a
  24 MP frame for the strongest profile of the test database.
- **`lensCost` is measured per combination** of corrections (distortion, lateral
  CA, vignetting) in `RenderProgress.cpp`, not a single weight.
- **Python's `lens` argument is optional on the buffer-form calls**, where the
  caller holds pixels and no `Photo`; it defaults to no correction. The
  `Photo` forms resolve it themselves.
- **Paste before the database has loaded** matches live at render time and
  records the lens on the next toggle, since a paste cannot wait on the
  database off the GUI thread.
- **GPU parity** (section 9) is measured on lavapipe only. No hardware GPU was
  available, so the tolerance is not confirmed on a real device.
- **Not yet done:** the hand check on the user's Canon 80D files (plan step 11),
  and packaging (`just rpm`, `rpm-smoke`, the `.deb`'s `liblensfun1`
  dependency) was written but not run here. See `docs/todo.md`.

The amendments named in the Consequences are made in ADRs 009, 012, 031, 044,
045 and 049, and in 022 and 030 for the history step and the saved sidecar.
