# Review: lens corrections, steps 0–10 (uncommitted, branch `lens-corrections`)

Reviewer: Claude Opus 5.5, 2026-10-18. Scope: `git diff` and the untracked files against
`docs/ideas/lens-corrections-plan.md` and ADR 050. No production code was changed.

## What was run

- `cmake --build build/container-debug` (lensfun 0.3.4, `ARRAW_WITH_LENSFUN=ON`): builds.
- `ARRAW_REQUIRE_LENSFUN=1 arraw-tests "[lens],[lensfun],[stage],[ladder],[digest]"`: 120 cases
  pass. `arraw-gpu-tests "[lens]"`: 12 cases pass. `arraw-widget-tests "[lens]"`: 10 cases pass.
- **Default-off digests, checked independently.** I built `HEAD` (17aaa6f) in a temporary
  worktree and wrote `ARRAW_DIGEST_OUT` for both trees. Without the new `until-lens` lines, the
  3220 CPU lines are byte-identical, and every `until-lens` hash equals its `until-denoise` hash.
  I did not check the GPU digest.
- Probes against the system lensfun database (Ubuntu, 0.3.4, stamp 2022-02-20). The results are
  behind finding 1.

## Findings, ranked

### 1. High: the calibration sampled is not the lens that matched (duplicate lensfun names)

`LensIdentity` assumes "two lenses never share both" maker and model (`include/LensMatch.h:15-16`).
The real database breaks this. It has **82 maker+model pairs that appear more than once**, each
with a different calibration crop factor (full frame vs APS-C) and sometimes different mount
lists. Examples are most Canon EF L zooms and many Sigmas:

```
Canon EF 24-105mm f/4L IS USM     crop=1      (GetLenses order: first)
Canon EF 24-105mm f/4L IS USM     crop=1.611  (second)
Sigma 10-20mm f/3.5 EX DC HSM     crop=1.534  (first), crop=1.613 (second)
```

On an 80D, lensfun's `FindLenses` scores the APS-C entry higher (51 against 48 for the FF one).
`matchLens` keeps only the identity (`distinct`, `src/core/LensMatch.cpp:96-107`). Then
`sampleLensfun` → `findLens` (`src/core/LensfunSource.cpp:507-516`) and `LensDatabase::lens`
(`:429-441`) return the **first entry with that name in `GetLenses()` order**. So an 80D with an
EF 24-105 f/4L gets the full-frame calibration, and a Sigma 10-20 gets the Nikon-body (1.534)
one. This is not what lensfun would pick. Recorded and chosen lenses go through the same
first-by-name lookup. The test fixture has no duplicate names, so no test catches this.

Fix options. All of them keep the name-only record the user decided on (2026-10-18).
- (a) In `findLens` and `lens()`, choose among same-named entries the one with the largest
  calibration crop ≤ the image's crop × 1.01, with mount fit as a tie-break. This mirrors
  lensfun's own preference. The recorded `cropFactor` already gives the image crop.
- (b) Carry the calibration crop (or the index from lensfun's scoring) in `LensMatch` and
  sample that `lfLens*` directly. Recorded lenses then still need (a).
- Add a fixture lens twice (crops 1.0 and 1.6, different calibrations), plus a test that the
  80D samples the 1.6 one. Fix the "never share both" comment.
- The picker (`LensDatabase::lenses`) also lists such lenses twice under one name.

### 2. Medium: a paste before the database lands copies the switches but records no lens

`MainWindow::pasteSettings` (`src/app/ui/MainWindow.cpp:1264`) calls
`withMatchRecorded(..., lensDatabase_)`. While the database loads, `lensDatabase_` is null, so
`withMatchRecorded` returns the switches unrecorded (`src/app/LensSupport.cpp:134-136`). The
switches are disabled while loading, but Paste is not. Such a photograph is matched live forever
and is never recorded. A later database update can then silently move it to another lens, which
is the case ADR 050 §6 exists to prevent. The plan's step 7 test list ("a Lens paste stays at one
decode") does not cover the race.
Options: (a) disable Paste (or only its Lens section) until the database loads; (b) record
lazily in `lensDatabaseLoaded` when the open photograph has switches on and no record (that
would be a separate step, or folded into the paste step's history entry); (c) queue the paste.

### 3. Medium: a correction the lens has no calibration for is dropped quietly in export and in the status line

`resolveLens` reports `LensNotCalibrated` as `Severity::Info`
(`src/core/LensResolution.cpp:170-172`). `ExportQueue` keeps only notices that are not Info
(`src/app/ExportQueue.cpp:114`). So if vignetting is on and the lens has no vignetting
calibration, the export says nothing about it. If *every* requested correction is missing, the
correction comes back inactive and carries no provenance. `resolveLensView` then shows
"No profile: No lens was found." (`src/app/LensSupport.cpp:120-128`), which is false: the lens
*was* found and the note line names the real reason. ADR 050 §6 asks for one notice that names
the reason for each correction that is on but unresolved.
Options: make it a Warning when a requested correction is skipped, or keep Info and have the
export and status paths treat it explicitly. Take the lens name from the match, not from the
correction's provenance.

### 4. Medium (design deviation, accepted with caveat): `lens=` is optional on the buffer overloads in Python

`develop` / `sample` / `curve_histogram` over an `ImageBuffer` take `lens=None`
(`src/python/BindPhoto.cpp:608, 667, 750`). ADR 050 §4 says "with no default" so that no caller
can skip a correction. `arraw.develop(photo.decode(), photo.state)` now silently renders
uncorrected, which is `old`'s defect, moved to Python. The `Photo` overloads resolve the
correction correctly. Options: (a) make `lens` keyword-required (`lens=arraw.LensCorrection()`
for "none"); (b) keep the default, but warn when the buffer was decoded from a RAW
(`decodedWith` is set) and no `lens` was given; (c) keep it, but record the deviation in ADR 050
and in the docstring. I lean to (a). It costs one keyword per call and keeps the C++ rule.

### 5. Low: the curve histogram leaves the GPU whenever a correction is active

`src/app/PreviewRenderer.cpp:593`: `onGpu = gpu && !lens.active() && ...`. This looks like a
leftover from the step-6 GPU refusal, which step 9 removed. `sampleOnGpu` takes the lens. The
results are correct but slower, and nothing tests it. Drop the condition, or add a comment saying
why it stays.

### 6. Low: Exiv2's ambiguous Canon ID names reach matching as one string

`makerNoteName` (`src/core/LensFactsReader.cpp:109-119`) adds `Exiv2::lensName(...).print()`
as-is. For a shared Canon `LensType` that Exiv2 cannot narrow, it prints
`"A *OR* B *OR* C"` (the `*OR*` literal is in libexiv2.so.28). lensfun's word score over the
whole string will usually fall below 40, so this is safe, but candidate 3 is lost exactly for
third-party lenses. Option: split on `" *OR* "` and add each part, letting the focal and
aperture filter and the ambiguity rule decide. (I confirmed that Exiv2 0.28 tries
`CanonCs.LensType` first in `lensName`, so the LensType name *is* the third candidate as planned.)

### 7. Low: a sidecar record with no `lensCropFactor` reads silently as 1.0

`src/core/Sidecar.cpp:809`: an absent key gives `std::optional{1.0}`. A hand-edited or
truncated record for an 80D would then render with crop 1.0 (a different scale) and no notice.
Options: treat it like a missing model (report it, no record), or fall back to the camera's
crop with a notice.

### 8. Low: a lens that does not match at toggle time is never recorded

`recordLensMatch` records nothing when matching fails (`src/core/LensResolution.cpp:223-227`).
That is right for a render, but when a later database *does* list the lens, the photograph
starts correcting on the next open with nothing recorded and no notice. The ADR's text supports
this ("No recorded lens: the facts are matched"). Say it explicitly in ADR 050 §6, or record on
first successful resolution in the app.

### 9. Low / notes

- `planFor(const Photo&, request)` (`src/core/ProcessingPlan.cpp:136`) loads
  `LensDatabase::system()` implicitly, so its notices are discarded. Only tests use it today. It
  is worth a doc line that it may block on the first load.
- Presets: `negativeToJson` writes the `"recorded"` lens into preset JSON. `withLook` ignores it
  on apply, which is correct, but presets now carry per-shot data. Consider leaving `recorded`
  out when writing a preset.
- `LensDatabase::fromSearch` loads a newer user update *over* a bundled copy. I checked that
  lensfun 0.3.4 replaces a same-name lens on the second `LoadDirectory` (the later load wins),
  so this is correct. The test uses different lens names and does not pin the replacement.
- The `HomeDataDir` (personal XMLs) can become the reported `directory`/`stamp` when it is the
  newest. Editing a personal calibration then raises `LensDatabaseChanged` on every recorded
  photograph. That is arguably right, but worth a line in `docs/development.md`.
- The CI Python job relies on `ARRAW_WITH_LENSFUN=ON` failing configure, and does not also set
  `ARRAW_REQUIRE_LENSFUN`. This is fine as long as the configure stays `ON`.
- The real 80D crop factor in lensfun is 1.613, not 1.6. Tests use the fixture's 1.6, which is
  fine. `info` on the system database prints 1.613.

## What is fine

- **Default off is bit-identical.** The digests match `HEAD` exactly (above), `Stage::Lens`
  collapses onto Denoise, and `keepsRung` skips it when collapsed.
- **No re-decode.** Lens settings stay out of `DecodeOptions`. `syncDecode` compares only them,
  and the widget tests count one decode across a toggle, an undo and a paste.
- **Ladder and plan.** `LensPlan` sits in `stagesOf`, so a toggle is stale by value from Lens
  on, while Denoise and the source survive. `sameAtTap` includes the lens. The brush frame sits
  in `BrushCoverageRef` and is compared by value (`sameBrushFrame`), so pointwise rungs are not
  spuriously stale. GPU resume and the resident Lens rung are tested.
- **CPU kernel vs lensfun.** The tables are sampled along the ray to the farthest corner from
  lensfun's own centre, which the test finds independently as the geometry's fixed point. TCA is
  sampled at the sensor position and vignetting at the sensor radius. The accuracy test compares
  against `ApplySubpixelGeometryDistortion` and `ApplyColorModification` per pixel, at corners,
  edge midpoints and a scatter (< 0.05 px at 129 nodes). Odd and half sizes and the fill scale
  are resolution-independent.
- **GPU parity.** `lens.frag` mirrors the CPU: pixel-centre offsets, the same tap base and
  clamp, ringing clamped only when every tap is non-negative, CA as `displacement + sensor·(s−1)`,
  and the centre split into whole and fraction for float precision. A per-stage comparison and an
  end-to-end comparison exist.
- **Frames.** Strokes stay in the sensor frame. Dab centres are mapped forward with
  `correctedFrom`, and the pointer goes back through `MaskViewMapping::sensorFrom`.
  `brushFrameOf` keeps only the distortion, centre, aspect and fill, so a vignetting or CA toggle
  keeps the coverage cache, while a distortion toggle (or a changed table) misses it. Linear and
  radial masks are untouched. Lens never writes the crop.
- **Matching.** Camera first, an exact whole-model camera match, the mount filter through
  lensfun's compat list, the focal and aperture filter, a threshold, exact-name precedence,
  near-tie → `LensAmbiguous`, and mountless search only with one survivor. On the system
  database, the 80D and its kit lens match.
- **Recorded lens.** It is looked up by exact name with no fuzzy step. A missing one gives
  `LensProfileMissing` and is not re-matched, and a stamp change gives an informational
  `LensDatabaseChanged`. This is name-only, as decided.
- **Sidecar.** The keys are written key by key under "differs from what reading gave", so
  unknown tokens and malformed values survive. The record is written and removed as a unit. The
  unknown-key list knows the new keys, and a sidecar with only lens keys gives a negative and no
  state.
- **Copy/paste.** `CopySection::Lens` carries the switches only and keeps the target's record,
  and the paste records the target's match. Lens is skipped between a RAW and a non-RAW. The
  QSettings migration is generalised and tested for both Demosaic and Lens.
- **App.** The database loads on a `jthread` declared last (joined first). Renders wait while a
  photograph asks for corrections the database must resolve. The `LensView` cache is keyed on
  the settings and the database. `ExportJob` resolves on the export thread. The thumbnail key
  adds a digest of the tables plus the stamp, but only when the correction is active, so existing
  keys are unchanged.
- **CLI.** `--lens-corrections`, `--lens` and `--lens-database` are applied over the sidecar, and
  `info` gives text and JSON. The database is not loaded for a batch with nothing on.
- **Build.** The tri-state is as in `old`: `ON` is a `FATAL_ERROR` naming the packages, `AUTO`
  and `OFF` degrade and report it, and the library is linked PRIVATE with a private define.
  `just release` on Linux passes `ON`, which `deb` inherits. The spec has `BuildRequires`,
  `Requires` and `ON`. CPack depends on `liblensfun-data-v1`, CI uses `ON` plus
  `ARRAW_REQUIRE_LENSFUN=1`, and the RPM smoke test checks the 80D kit lens.
- **Tests.** Broadly they pin the invariants: bit-exact identity, toggle and edit resume points,
  stale-by-value, recorded-vs-matched, notices by reason, frames, cache keys, and the
  decode count. The gaps are findings 1, 2, 3 and 5, and the absence of an automated digest
  comparison (it remains a hand check, done here for the CPU).

## Verdict

The implementation is solid and follows the plan closely. It is **not ready to merge until
finding 1 is fixed**: on the reference body with EF L or Sigma lenses it samples a different
calibration than the one lensfun matched, and the fixture hides this. Findings 2 and 3 should be
fixed in the same round. Finding 4 needs a user decision (keep the default and document it, or
require `lens=`). The rest can follow.
