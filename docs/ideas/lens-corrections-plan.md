# Lens corrections (Phase 2 of the negative settings) — plan

Status: proposed, 2026-10-17. Decision record:
[ADR 050](../adr/050-lens-corrections-are-a-stage-between-noise-reduction-and-the-chain.md).
Follows [negative-settings-plan.md](negative-settings-plan.md) ("Phase 2
sketch", open questions 7 and 8) and
[ADR 049](../adr/049-settings-of-the-negative-sit-beside-the-develop-state.md).

## Decisions

| Question | Answer | By |
|---|---|---|
| Default for new photographs | Off | user, 2026-10-17 |
| Stage placement | (c) cached `Stage::Lens` after Denoise, before Pointwise | user, 2026-10-17 |
| The automatic match | Recorded in the sidecar (lensfun maker and model, crop factor, origin, database stamp) | user, 2026-10-17 |
| Lens data source (v1) | lensfun only, matched from the EXIF as `old` did, `old`'s faults fixed. Embedded readers later and optional. Replaces "embedded, then lensfun" (2026-10-16) | user, 2026-10-17 |
| Source field | Dropped in v1; `arraw:lensSource` arrives with the first embedded reader (ADR 050 §8) | proposed |
| lensfun dependency | `old`'s tri-state `ARRAW_WITH_LENSFUN`: `ON` for Linux release builds and CI, `AUTO` in development, Windows and macOS deferred | user, 2026-10-17 |
| Formats | RAW only | user, 2026-10-17 |
| Presets and copy/paste | `CopySection::Lens` (the switches; never the recorded lens) | user, 2026-10-16 |
| Reference body | Canon EOS 80D, CR2, crop 1.6 | user, 2026-10-17 |
| Frames | ADR 009 as written; the brush is drawn through a forward map | proposed |
| Near-tie match | Nothing applied, `LensAmbiguous`, "Choose lens…" | proposed |
| Lens not found | Switches kept; the correction is skipped with a notice naming the reason; export goes ahead | proposed |

## What the code does today (verified 2026-10-17)

- `rawimport::load`:
  - gives `RgbaU16`, clipped (`highlight = 0`), in sensor geometry
    (`user_flip = 0`, whose comment already names lens profiles as the reason);
  - a half-size decode has `pixelScale` 2, and its size is `floor(size / 2)`.
- `ProcessingPlan` has `decode` (a header) and five stage groups. `Stage` is
  public (`include/RenderCheckpoint.h`), and Python binds it by name
  (`BindSettings.cpp`). `ProgressStep` is public too.
- Every render entry point takes `(source, DevelopState, request)`:
  `Develop.h`, `GpuDevelop.h`, `PreviewRenderer::request(DevelopState, …)`,
  Python, and `ExportCommand.cpp`. None of them can see `Photo::negative()`.
- `MainWindow::syncDecode` re-decodes when `decodeOptionsFor(photo)` differs
  from what the shown pixels were decoded with. Lens settings stay out of
  `DecodeOptions`, so they never re-decode.
- `ThumbnailWorker::developThumbnail` develops a half-size decode with the
  sidecar's **state only**.
- `sensorFrom(CorrectedPosition)` (`DevelopedFrame.cpp`) is the identity.
- The geometry pass interpolates bilinearly (ADR 014). `RowBands.h` is the CPU
  banding helper.
- `ExifInfo` (`include/ExifInfo.h`) has `make`, `model`, `lensModel`
  (`Exif.Photo.LensModel`, else `Exiv2::lensName`), `fNumber`, `focalLength`
  and `focalLengthIn35mmFilm`. It is display-only (ADR 028). It has no focal
  range, no maximum aperture, no focus distance and keeps one lens string.
- **CR2 lens data**, checked against Exiv2 0.28.8's tag table and LibRaw
  0.21.5's headers here, not yet against an 80D file:
  - identity: `Exif.Image.Make`/`Model`, `Exif.Photo.LensModel`,
    `Exif.Canon.LensModel` (0x0095), `Exif.CanonCs.LensType` (numeric, named by
    Exiv2), `Exif.CanonCs.Lens`/`ShortFocal`/`FocalUnits`,
    `Exif.CanonCs.MaxAperture`/`MinAperture`; LibRaw's `lens.makernotes`
    `LensID`, `MinFocal`, `MaxFocal`, `MaxAp4CurFocal`, `CurFocal`,
    `FocalLengthIn35mmFormat`;
  - distance: `Exif.CanonFi.FocusDistanceUpper`/`Lower`;
  - corrections: only the camera's switches,
    `Exif.CanonVigCor2.PeripheralLightingSetting`,
    `ChromaticAberrationSetting`, `DistortionCorrectionSetting`,
    `Exif.CanonLiOp.PeripheralIlluminationCorr` and `DigitalLensOptimizer`.
    `Exif.Canon.VignettingCorr` (0x4015) is an undecoded array (exiftool reads
    flags, strengths and the original size from it, as far as we know, no
    coefficients). LibRaw reads nothing of these.
  - **Conclusion, stated with that caveat: a CR2 carries no usable profile.**
    Step 11 confirms it with `exiftool -v3` on the user's files.
- **lensfun is absent**: `pkg-config lensfun` fails here, `/usr/share/lensfun`
  does not exist, and `CMakeLists.txt`, `CMakePresets.json`, `Justfile`,
  `tools/ci/install-deps.sh` (shared by CI and the sandbox image),
  `tools/sandbox/Containerfile`, `packaging/fedora/arraw.spec`,
  `tools/package_fedora.sh`, `tools/smoke_fedora_rpm.sh`,
  `.github/workflows/ci.yml`, `README.md` and `docs/development.md` do not name
  it.
- There is **no AppImage and no Windows package** in this repo.
  `packaging/linux` holds only the desktop file and the metainfo. The `.deb`
  comes from CPack (`CMakeLists.txt`, `CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON` and
  an explicit `CPACK_DEBIAN_PACKAGE_DEPENDS`), built by `just deb`, which runs
  `just release` (the cross-platform `release` preset).
- The sidecar writes booleans as `True`/`False`. `negativeToJson` gives
  `{"demosaic": …}`. `CopySection::Vignette` is the creative effect.

## The `old` implementation (origin/old, ADR 0032), evaluated

Read with `git show origin/old:…`. The files are `docs/adr/0032-…`,
`src/pipeline/LensCorrection.*`, `src/pipeline/LensfunSource.*`,
`tests/fixtures/lensfun-mini/mini.xml`, `tests/test_Lensfun*.cpp`,
`CMakeLists.txt:129-177`, `packaging/fedora/arraw.spec`,
`packaging/linux/{apt-build-deps.txt,build-appimage.sh}`,
`.github/workflows/{release,windows-package}.yml`, `.devcontainer/Containerfile`,
`Justfile` (`lensfun-db`) and `tools/dev_lensfun_db.py`.

**Kept:**

- three profile-driven switches and no sliders, so the correction is computed
  once and cached;
- one source-independent radial model with a centre;
- one resample for everything (vignetting is a gain, red and blue add their CA
  scale to green's distortion);
- `lfModifier` resolving lensfun's own models;
- the auto-fill, so the corrected frame has no blank corners;
- headless tests against a small fixture database;
- **the dependency management**, mapped below.

**Replaced, because tied to the old architecture:** `DevelopSession` applied the
correction to its own buffers (so batch export skipped it); toggles lived in
`GlobalAdjustment`; Qt types in the pipeline; `defaultCrop` set to the inscribed
rectangle; masks and spots in corrected pixels.

**Faults, each with its fix in v1:**

| `old` | v1 | Step |
|---|---|---|
| `FindCameras` → `cams[0]`, `FindLenses` → `lenses[0]`; no score; a missing camera passes `nullptr`, which matches across mounts | camera first, mount and crop from it; score threshold; focal-range and aperture filter; near-tie → `LensAmbiguous`; mountless search only when one lens survives | 3 |
| one lens string (`LensModel`) | ordered candidates, including Canon's maker-note names | 3 |
| `lfDatabase` built per photograph, no stamp | one process-wide `LensDatabase`, `stamp()` | 2 |
| bundled database overrides `lensfun-update-data` | user update preferred when newer, as lensfun does | 2 |
| 0.3.x API only, no version check | `LF_VERSION` checked at compile time; a 0.3.95 branch if a platform ships it | 1, 2 |
| table sampled along one diagonal from the geometric centre; lensfun uses its calibrated centre | sampled from lensfun's centre | 4 |
| 64 linear nodes, accuracy never measured | node count chosen by measurement (< 0.05 px) | 4 |
| lateral CA looked up at the corrected radius | at the sensor radius | 4 |
| bilinear, edges clamped | Catmull-Rom | 4 |
| fill zoom by binary search over each buffer's boundary pixels, so preview and full size differ | one fill scale in normalised coordinates, stored in the plan | 4 |
| aperture f/8 and distance 1000 assumed silently | assumed values marked in the provenance | 4 |
| a missing profile is a silent no-op | a notice naming the reason, shown in the app and `info` | 6, 7 |
| the caller applies it, so export could skip it | a plan stage | 6 |
| claimed `crs:LensProfile*` round-trips | not claimed | — |

### Dependency management, mapped onto this repo

| Concern | `old` (verified) | Here |
|---|---|---|
| CMake | `CMakeLists.txt:129-177`: `ARRAW_WITH_LENSFUN` `AUTO`/`ON`/`OFF`; `pkg_check_modules(LENSFUN IMPORTED_TARGET lensfun)`, else `find_library(lensfun)` + `find_path(lensfun.h PATH_SUFFIXES lensfun)` + glib-2.0, gobject-2.0, intl, iconv; `FATAL_ERROR` naming `lensfun-devel`/`liblensfun-dev` when `ON`; linked PUBLIC with a PUBLIC `ARRAW_HAS_LENSFUN` | `CMakeLists.txt`, after the exiv2 block: the same tri-state and lookup, into an `arraw-lensfun` interface target; linked PRIVATE to `arraw` beside `${ARRAW_EXIV2}`; `ARRAW_HAS_LENSFUN` PRIVATE to `arraw` and the tests; `lensfunAvailable()` in the public API |
| Release builds | `ON` in each packaging script | `Justfile`: `release` split into `[linux]` (passes `-DARRAW_WITH_LENSFUN=ON`) and the rest (`AUTO`); `deb` inherits it. *Alternative:* a `release-linux` preset with a host `condition` in `CMakePresets.json` |
| Fedora | spec: `BuildRequires: pkgconfig(lensfun)`, `Requires: lensfun` (with the comment that the database is data), `-DARRAW_WITH_LENSFUN=ON` | `packaging/fedora/arraw.spec`: the same three, and `%changelog`; `tools/package_fedora.sh`: `lensfun-devel` in `required_packages` |
| Debian/Ubuntu | `packaging/linux/apt-build-deps.txt`: `liblensfun-dev`, `liblensfun-data-v1` | `tools/ci/install-deps.sh`: both; `CMakeLists.txt` CPack: `liblensfun-data-v1` added to `CPACK_DEBIAN_PACKAGE_DEPENDS` when lensfun is built (SHLIBDEPS finds `liblensfun1`); `docs/development.md` and `README.md` apt lines |
| Dev container | `.devcontainer/Containerfile` (Fedora): `lensfun-devel` | `tools/sandbox/Containerfile` (Ubuntu 26.04) installs from `tools/ci/install-deps.sh`, so step 0 is one edit there, then `just sandbox-build` |
| AppImage | `build-appimage.sh` copies `/usr/share/lensfun/version_1/*.xml` to `AppDir/usr/share/lensfun/db`; `release.yml` fails if `liblensfun*.so*` or an XML is missing | no AppImage here. The lookup supports the layout from step 2; the staging and the check are written down for when an AppImage exists |
| Windows | vcpkg `lensfun`; `tools/dev_lensfun_db.py` + `just lensfun-db` `[windows]` copies the database from vcpkg's buildtrees to `build/lensfun/db`; `windows-package.yml` checks `lensfun.dll` and `lensfun\db\*.xml` and runs `[lensfun]` | deferred: `AUTO`; `docs/development.md` names `lensfun` as an optional vcpkg port. The script and recipe are ported when Windows packaging exists |
| macOS | Homebrew `lensfun` | deferred: `AUTO` |
| Database lookup | `LensfunSource.cpp`, `bundledLensfunDbDir()`: explicit path; `applicationDirPath()/../share/lensfun/db`; `…/lensfun/db`; else `lfDatabase::Load()` | `src/core/LensfunSource.cpp`: the same order with `std::filesystem` and an `executableDirectory()` helper (`/proc/self/exe`, `GetModuleFileNameW`), no Qt; loaded once; the user's update directory wins over a bundled copy when newer |
| CI | Windows: `[lensfun]` tests | `.github/workflows/ci.yml`: both jobs configure with lensfun required and run with `ARRAW_REQUIRE_LENSFUN=1`, so `[lensfun]` tests fail rather than skip, as `ARRAW_REQUIRE_CLI` does for Python |

## Matching a Canon lens

The inputs for an 80D CR2 are listed above. Expected lensfun entries (to be
checked in step 0 against the installed database): camera `Canon` / `Canon EOS
80D`, mount `Canon EF-S`, crop 1.6; EF-S is compatible with EF in lensfun's
mount table, so EF lenses are found too.

**Candidates, in order:**

1. `Exif.Photo.LensModel`. Canon writes it without the maker and without a
   space after the mount (`EF-S18-135mm f/3.5-5.6 IS USM`). lensfun's fuzzy
   comparison works on words; whether it joins this to `Canon EF-S 18-135mm
   f/3.5-5.6 IS USM` is tested in step 3. If it does not, the candidate gets a
   normalised twin (`EF-S18` → `EF-S 18`).
2. `Exif.Canon.LensModel`, when it differs.
3. Exiv2's name for `Exif.CanonCs.LensType`. Exiv2 already narrows shared IDs
   by the focal range and aperture.

**Filters**: the focal range (`CanonCs.ShortFocal`/`Lens`, LibRaw `MinFocal`/
`MaxFocal`) and the maximum aperture must agree with the lensfun lens; the
mount must fit the camera; the score must pass a threshold. Two distinct
survivors within a margin give `LensAmbiguous`.

**Third-party lenses.** Sigma and Tamron on Canon report a Canon lens ID, often
shared with other lenses (Canon's own or other third parties'). The body's
`LensModel` for such a lens can be a Canon name, a bare focal range, or empty.
What the 80D writes for any given lens is unknown until we see files. The
design therefore never trusts one string: an ID-derived name that names the
wrong maker is caught by the focal and aperture filter or falls into the
ambiguity rule, and the photographer's "Choose lens…" choice is recorded. The
lens maker is never a filter: Canon bodies do not reliably write `LensMake`.
Adapted manual lenses report no lens and give `LensUnidentified`.

## Stage placement and frames

Decided (user, 2026-10-17): (c), a cached `Stage::Lens` after Denoise. The
comparison with (a) "in the decode" and (b) "distortion in Geometry" is in ADR
050's rejected options. Its one real disadvantage is the second resample when
straightening; step 6 measures it, and (b)'s idea (fold the distortion into the
geometry warp, keep CA and vignetting in Lens) is the upgrade if it matters.

Frames follow ADR 009 (ADR 050 §7): crop in the upright frame and never written
by the lens; linear and radial masks in the corrected frame, unchanged; brush
strokes in the sensor frame, drawn through a forward map of each dab centre
(*alternatives:* warp a sensor-frame raster, or sample the coverage at `L(c)` in
the chain); spots later, in the sensor frame, as a stage before Lens.

## Steps

Each step builds, passes `just test` and `just format-check`, and is reviewed
into `docs/reviews/` before the next. Sonnet implements and Opus reviews, as in
Phase 1. Steps 0 to 4 are the dependency, the matching and the correction, and
need nothing from the app. Steps 8 to 10 can be reordered after step 6.

### 0. lensfun in the dev image and the sandbox (first)

- `tools/ci/install-deps.sh`: add `liblensfun-dev` and `liblensfun-data-v1`
  (Ubuntu's names for `old`'s `lensfun-devel` and the data). One edit covers CI
  and `tools/sandbox/Containerfile`. `liblensfun-bin` (`lensfun-update-data`)
  is not needed.
- The user rebuilds the image (`just sandbox-build`); an agent inside the
  sandbox cannot.
- Record in this plan, from the installed packages: the lensfun version on
  Ubuntu 26.04 and Fedora 44 (0.3.x or 0.3.95), the database directory
  (`version_1` or `version_2`) and its stamp, and whether the database has
  `Canon EOS 80D` and which EF/EF-S lenses, Sigma and Tamron included.
- **Check:** `pkg-config --modversion lensfun` succeeds in a fresh sandbox, and
  `/usr/share/lensfun/version_1/*.xml` exist.

### 1. Build and packaging plumbing (no feature)

- `CMakeLists.txt`: the tri-state option and lookup (table above), with the
  comment LibRaw and exiv2 carry, saying why release builds use `ON`;
  `LF_VERSION` reported in the configure message; the CPack dependency.
- `Justfile`: `release` passes `ON` on Linux.
- `packaging/fedora/arraw.spec`, `tools/package_fedora.sh`: as the table.
- `.github/workflows/ci.yml`: `-DARRAW_WITH_LENSFUN=ON` on configure (the
  Python job through `just py-test`'s configure, by an environment variable the
  recipe forwards) and `ARRAW_REQUIRE_LENSFUN=1`.
- `src/core/LensfunSource.cpp` exists with only `lensfunAvailable()` and
  `lensfunVersion()`; `arraw-cli --version` prints `lensfun 0.3.x` or
  `lensfun: not built`.
- `README.md`, `docs/development.md`: the apt, dnf and vcpkg lines; the
  dependency table gains lensfun (LGPL-3) and its database (CC BY-SA 3.0).
- **Tests:**
  - configure with `ON` and no lensfun fails with the package names (checked
    by hand, recorded in the review);
  - `OFF` builds and `just test` passes, with `[lensfun]` tests skipped;
  - `--version` prints the lensfun line;
  - `just rpm` builds, and `just rpm-smoke` installs it with `lensfun` pulled
    in; `just deb`'s control file depends on `liblensfun1` and
    `liblensfun-data-v1` (`dpkg-deb -I`).

### 2. The database, loaded once

- `src/core/LensfunSource.{h,cpp}`, the only includer of `<lensfun.h>`.
  `LensDatabase` (public, no lensfun type):
  - `system()`: lazy, once, thread-safe, never on the GUI thread; the lookup
    order of ADR 050 §5;
  - `fromDirectory(path)`;
  - `stamp()`, `directory()`, `cameraCount()`, `lensCount()`;
  - `lenses(query, mountOf(camera))` for the picker and the CLI;
  - lookups behind a mutex.
- `arraw-cli lenses [--database DIR] [--camera TEXT] [--search TEXT]`, and
  `lenses --info` printing the directory, stamp and counts. It is the tool for
  the hand check and the RPM smoke test.
- **Fixture:** `tests/fixtures/lensfun/` (below).
- **Tests:** `[lensfun]`, skipped when not built:
  - `fromDirectory` loads the fixture, counts and stamp as written;
  - the lookup order with temporary directories: explicit beats bundled beats
    system; a newer user update beats a bundled copy;
  - `system()` is constructed once across threads (a counting hook);
  - a missing directory gives `LensDatabaseMissing`, not a throw.
  - `tools/smoke_fedora_rpm.sh` runs `arraw-cli lenses --camera "Canon EOS 80D"`
    on the system database and expects a hit.
- **Measure:** load time and RSS of the system database, cold and warm.

### 3. Lens facts and matching

- `LensFacts` in `ImageMetadata`, for a RAW, filled by `readImageMetadata`:
  camera strings (EXIF, then LibRaw's), the lens candidates (above), focal
  range, maximum aperture, focal length, aperture, focus distance, 35 mm focal
  length. `ExifInfo` stays display-only (ADR 028).
- `matchLens(const LensFacts&, const LensDatabase&) -> LensMatch | notice`:
  camera first, the filters, the threshold and the ambiguity margin; the crop
  factor from the camera, else `f35 / f`; a prime with no focal length takes
  its own; a zoom with none gives `LensFocalUnknown`.
- **Tests**, against the fixture database, with `LensFacts` built in code (no
  RAW needed for the logic):
  - Canon: `EF-S18-135mm f/3.5-5.6 IS USM` on `Canon EOS 80D` matches the
    18-135 at crop 1.6; an EF prime matches on the EF-S body;
  - the candidate order: a missing `LensModel` falls back to the maker-note
    names;
  - third party: a Sigma named by `LensModel`; a Sigma whose ID-derived name is
    a Canon lens with another focal range is filtered out; two Sigmas sharing
    an ID and focal range give `LensAmbiguous`;
  - a Nikon F lens with the same name as a Canon one is refused by mount;
  - an unknown camera with `focalLengthIn35mm` gives the crop; with two
    surviving lenses it gives `LensAmbiguous`;
  - `LensUnidentified`, `LensNotInDatabase`, `LensFocalUnknown`;
  - one end-to-end read: a synthetic DNG from `make_raw_fixtures.py` with
    `Make=Canon`, `Model=Canon EOS 80D`, `LensModel=EF-S18-135mm…`, focal and
    aperture, gives the expected `LensFacts` and match.
- **Measure:** `readImageMetadata` with `LensFacts` per file, against today's;
  if the film strip feels it, the facts are read lazily.

### 4. The correction model, lensfun's sampling and the CPU kernel

- `include/LensCorrection.h`: tables, lensfun's centre, provenance, `active()`;
  `sensorFrom(const LensCorrection&, CorrectedPosition)`;
  `correctedFrom(const LensCorrection&, SensorPoint)` (Newton, reports
  failure); `fillScaleOf(const LensCorrection&, double aspect)`.
- `sampleLensfun(const LensMatch&, const LensFacts&, ImageSize, which)` in
  `LensfunSource.cpp`: `lfModifier` per correction, sampled from its centre at
  the photograph's aspect.
- `src/core/Lens.cpp`: `applyLens(view, LensPlan) -> RgbaF32`, banded with
  `RowBands`, Catmull-Rom, the vignetting-only and CA-only shortcuts.
- **Tests:**
  - **Accuracy against lensfun**: at the fixture's strongest profile, the table
    against `lfModifier`'s per-pixel answer, under 0.05 px at every corner and
    edge midpoint (an off-centre calibrated centre in the fixture catches
    `old`'s diagonal fault). This picks the node count;
  - **Interpolation**: a zoom between its two calibrated focal lengths;
  - **Kernel on generated images**: a barrel and a pincushion grid come out
    straight (residual < 0.05 px); pincushion fills with no sample outside; a
    colour-split edge realigns; a known falloff becomes flat to 1e-4; values
    above 1 survive; an identity correction is bit-exact;
  - **Exactness**: `correctedFrom∘sensorFrom` within 1e-4 px;
  - **Resolution independence**: the full-size result halved matches the
    half-size input's result, odd sizes included;
  - **Fill**: the same fill scale at three resolutions.
- **Measure:** the Lens pass at 24 MP with 8 threads for bilinear, Catmull-Rom
  and Lanczos-3, and per combination; the same at the preview level and
  thumbnail size; table error for 64, 128, 256 nodes.

### 5. The settings, the recorded lens, persistence and copying (no pixels)

- `LensSettings`, `RecordedLens`, `LensOrigin` (tokens `matched`, `chosen`) in
  `include/NegativeSettings.h`; `NegativeSettings::lens`, `validate`,
  `defaultNegativeFor` (off).
- `recordLensMatch(const Photo&, const LensDatabase&) -> Photo`: records a
  match only when a correction is on and nothing is recorded.
- **Sidecar** keys of ADR 050 §8, read with "an unknown token gives a notice and
  the default", written by "differs from what reading gave"; the unknown-key
  list in `Sidecar.cpp` learns them. **JSON**: `"lens": {"distortion": true,
  …, "recorded": {"maker": …, "model": …, "cropFactor": 1.6, "origin":
  "matched", "database": …}}`.
- **History**: `ChangeDescription::negative` keys; `HistoryModel` words them
  ("Lens: distortion on", "Lens: Canon EF-S 18-135mm…").
- **Copying**: `CopySection::Lens` (`"lens"`) in `defaultCopySections`, not
  applicable to a non-RAW, the `QSettings` migration as for Demosaic; `withLook`
  carries the switches, keeps the target's record, and the paste records the
  target's match.
- **Tests:** token round-trip; absent keys mean off; an unknown token survives
  a write that changes nothing; a sidecar with only lens keys records a negative
  and no state; recording happens once, survives switching off, and undoes with
  the toggle; a paste never copies the record and records the target's own; RAW
  to JPEG skips Lens; `syncDecode` sees no change.

### 6. `Stage::Lens`, resolution, and the CLI end to end

- `Stage::Lens`, `ProgressStep::Lens`, the table row (`runs` =
  `plan.lens.active()`), `stagesOf`, `keepsRung`; `LensPlan` from
  `lensPlanFor(const LensCorrection&, ImageSize)`.
- The public signatures gain `const LensCorrection&` with no default.
  `planFor(photo, …)` calls `lensCorrectionFor`. `GpuDevelop` refuses an active
  lens block with a reason. Python's `Stage.LENS`.
- `resolveLens(const LensSettings&, const LensFacts&, const LensDatabase&,
  ImageSize, DiagnosticLog&)` and `lensCorrectionFor(const Photo&, const
  LensDatabase&, DiagnosticLog&)`; the notices of ADR 050 §6 in
  `Diagnostics.h`.
- **CLI**: `export --lens-corrections distortion,vignetting,ca|all|none`,
  `--lens "Maker|Model"`, `--lens-database DIR`, applied on top of the sidecar
  as `--demosaic` is. `info` prints a "Lens:" line (identity, recorded or
  matched live, crop, database stamp), one line per correction with its
  provenance, and the notices; `--json` adds `"negative"."lens"` and
  `"lensCorrection"` (provenance only).
- **Tests:**
  - **Digests**: `[.digest]` render digests are identical with lens off;
  - **Ladder**: a toggle resumes from the Denoise rung, an exposure edit from
    the Lens rung; plans differing only in the Lens block are stale by value;
  - **API**: `stopAfter = Lens` returns the corrected frame; "no plan member
    outside `stagesOf` but `decode`" holds;
  - **Recorded lens**: with a fixture database where a second, better-scoring
    lens is added, a recorded match keeps its lens; a renamed lens gives
    `LensProfileMissing`; a changed stamp gives `LensDatabaseChanged`;
  - one notice per unresolved photograph, naming the reason;
  - **CLI**: export with distortion on differs from off (Phase 1's "silent
    wrong pixels" pin, for lens); flags override the sidecar; `info` text and
    JSON against the fixture database; built without lensfun, `info` says
    `LensfunUnavailable`.
- **Measure:** slanted-edge MTF50 for lens alone, lens plus a 2° straighten,
  and no lens. This decides whether (b) is ever needed.

### 7. App

- **The Negative group** gains "Lens corrections": checkboxes for Distortion,
  Vignetting and Chromatic aberration; a status line ("lensfun: Canon EF-S
  18-135mm…, recorded", "No profile: lens not in database"); "Choose lens…"
  (searchable, filtered by the camera's mount, recorded as `chosen`) and "Reset
  to automatic". Disabled for a non-RAW, in the crop mode, and until the
  database has loaded ("Loading lens database…").
- **The flow**: `setNegative` (with `recordLensMatch`) is followed by a render
  request with the new correction, no decode. The open photograph caches its
  `LensCorrection` per `LensSettings`. `LensDatabase::system()` loads in the
  background at startup (ADR 043).
- **Thumbnails**: `ThumbnailWorker` reads the sidecar's negative and passes
  `lensCorrectionFor`; `developedKey` adds the resolved correction's digest.
- **Export**: `ExportJob` resolves from its negative.
- The creative effect's label becomes "Post-crop vignette". The About dialog
  names lensfun, its version, the database stamp and the CC BY-SA attribution.
- **Tests:** with the counting `PhotoLoader::Decoder`, a toggle, its undo and a
  Lens paste stay at one decode; a toggle renders through the Denoise rung;
  thumbnail keys change with a toggle and a stamp; an export queued after a
  toggle uses it; the status says "No profile" for an unknown lens; the
  switches are disabled until the database lands.
- **Measure:** toggle latency to the first new picture, and the GUI thread's
  longest gap.

### 8. Brush and mask frames

- `sensorFrom` and `correctedFrom` take the correction; the single-argument
  form goes. `BrushPainting` maps the pointer through the lens;
  `BrushCoverage` draws dabs at `correctedFrom(lens, point)`; the
  `BrushCoverageCache` key gains the `LensPlan`. `DevelopedFrameMap` stays
  affine (ADR 044).
- **Tests:** a stroke at a corner feature lands on the same sensor point with
  distortion on and off, and its coverage follows the subject; a linear mask's
  50 % contour is straight in the corrected frame; pointer → sensor → corrected
  round-trips; a toggle invalidates the coverage cache, a develop edit does not.

### 9. GPU pass

- `lens.frag` with the tables in a small texture, the same Catmull-Rom;
  `GpuDevelop` drops the refusal.
- **Tests:** a `[gpu]` per-stage comparison against the CPU on a generated
  frame with all three corrections, the tolerance fixed from the measured
  difference; ladder resume on the device.

### 10. Python

- `arraw.LensSettings`, `arraw.RecordedLens`, `NegativeSettings(lens=…)`;
  `arraw.LensDatabase.system()` / `.from_directory()`;
  `arraw.record_lens_match(photo, db)`; `Photo.lens_correction(database=None)`;
  `develop(…, lens=…)` required, `Photo.develop` resolving it; `.pyi`
  regenerated.
- **Tests:** `tests/python`, mirroring steps 3, 5 and 6.

### 11. The hand check on real CR2 files

Needs the user's files (below). Commit no RAW: commit only the lens strings
(from `exiftool -s -LensModel -LensType -Lens -MaxAperture`) as a matching test
table, and record the visual checks in the review.

For each file:

1. `exiftool -v3` confirms that no correction coefficients are present (only
   the switches), or shows otherwise, which changes the "Later" list.
2. `arraw-cli info` names the right lens, crop 1.6, and a focal length and
   aperture equal to the EXIF.
3. **Distortion**: a straight edge near the frame's long side (door frame,
   horizon, building) is straight after correction, and the result lines up
   with the in-camera JPEG shot with distortion correction on (within 1 px at
   the corners) or with DPP's output.
4. **Vignetting**: a wide-open shot of an even sky or wall has even corners.
5. **Lateral CA**: a high-contrast edge in a corner loses its red/cyan fringe.
6. **Toggle**: crop and masks stay in place, a brush stroke stays on its
   subject.
7. **Third-party lens** (if one exists): matched, ambiguous, or not found, and
   "Choose lens…" fixes it.
8. **Clipped corner**: a clipped sky in a corner, vignetting on, does not show a
   band.

### 12. Docs

- The amendments in ADR 050's consequences: 009, 012, 031, 044, 045, 049 §7.
- `docs/todo.md`; `desired-features.md`; the spec's `%changelog`.
- `docs/development.md`: lensfun on each platform, `ARRAW_WITH_LENSFUN`, and
  how to point `--lens-database` at a test database.

### Later, optional (not v1)

- **Embedded readers**, each with real files and the overlay check: DNG
  `OpcodeList3` `WarpRectilinear`/`FixVignetteRadial` (specified, fixtures can
  be generated); Sony `DistortionCorrParams` and siblings and Fujifilm's three
  tags (both in Exiv2's tag table, reverse-engineered layouts); Panasonic, OM,
  Nikon Z from samples. The first one adds `arraw:lensSource`
  (`auto|embedded|lensfun`) with the migration of ADR 050 §8.
- **Windows**: vcpkg `lensfun`, the database script and `just lensfun-db`
  `[windows]`, a package check for `lensfun.dll` and `lensfun\db\*.xml`.
- **macOS**: Homebrew `lensfun`.
- **AppImage**: the database staging and its CI check.
- JPEG and TIFF correction (lensfun can, but their in-camera processing may
  already have corrected).

## Fixtures

No 80D sample exists in the repo, and RAWs are not committed.

- **`tests/fixtures/lensfun/canon-mini.xml`**, written by hand in `old`'s
  `mini.xml` format (`<lensdatabase version="1">`). Calibration values are
  invented, so no upstream data or attribution is involved:
  - mounts `Canon EF`, `Canon EF-S` (compatible with EF), `Nikon F`, and `old`'s
    `arrawtest`;
  - camera `Canon EOS 80D`, EF-S, crop 1.6; `old`'s `ArrawTest TestCam`, crop
    1.5;
  - `Canon EF-S 18-135mm f/3.5-5.6 IS USM`: a zoom with distortion, TCA and
    vignetting at 18 and 135 mm, and an off-centre calibrated centre;
  - `Canon EF 50mm f/1.8 STM`: an EF prime;
  - `Sigma 18-35mm f/1.8 DC HSM [A]` on Canon EF and Nikon F;
  - two Sigma zooms with the same focal range and aperture (the near-tie);
  - a Nikon F lens whose name collides with a Canon one (the mount refusal);
  - `old`'s `TestLens 50mm F2.8` with its strong barrel and falloff, for the
    kernel tests.
- **A copy with a changed stamp and a renamed lens**, for the recorded-lens
  tests, generated by the test from the first rather than committed.
- **A synthetic DNG** from `make_raw_fixtures.py` with Canon EXIF strings, for
  the one end-to-end facts test. Canon maker notes cannot be generated, so the
  maker-note candidates are tested on `LensFacts` built in code and, after
  step 11, on the real strings.

## Risks

- **A wrong lens is worse than none.** Mount, focal and aperture filters, a
  threshold, the ambiguity rule, visible provenance and the manual choice cover
  it. Third-party lenses on Canon are the likeliest failure, and nothing is
  known about the 80D's strings until files arrive.
- **lensfun's fuzzy match on Canon's spelling** (`EF-S18-135mm`) may not join;
  step 3 tests it, with a normalised twin as the fallback.
- **The database may lack a lens**, or an Ubuntu 26.04 database may be older
  than Fedora's. `LensNotInDatabase` says so; `lensfun-update-data` is the
  user's remedy and the lookup honours it.
- **Different databases, or a revised calibration, render the same recorded
  lens differently.** `LensDatabaseChanged` and the stamp in `info` show it. Full
  pixel reproducibility needs the coefficients pinned (open question 1).
- **A development build without lensfun renders differently.** It says so per
  photograph and in `--version`; CI and every package build require it.
- **0.3.x against 0.3.95**: if a supported platform ships 0.3.95 or a version 2
  database, the branch must be built and tested in CI there, not only compiled.
- **The `Stage` and signature churn** touches the CPU, the GPU, the app, Python
  and every test. Step 6 lands as one change, the GPU refusal keeping the two
  backends honest.
- **Ladder memory**: a float Lens rung of source size, about 384 MB at 24 MP.
- **Sharpness**: two resamples when straightening; measured in step 6.
- **GPU and CPU disagree** on a nonlinear warp near the edges; a per-stage test
  with a measured tolerance.
- **Highlights**: a gain above 1 lifts clipped corners above the clipped
  centre; checked by eye (step 11, item 8).
- **Corner noise**: vignetting after noise reduction amplifies it by the gain,
  the accepted trade of (c); noted in the UI help.
- **lensfun's thread safety** is undocumented: lookups are serialised, and
  resolutions are rare and cached.
- **Tests reading the system database**: they never do, except the labelled
  smoke checks; everything else uses `fromDirectory`.
- **Recording at paste time** reads each target's metadata; measured with a
  multi-photo paste once that exists.

## What to measure

- **lensfun**: system database load time and RSS; resolve time per photograph.
- **Matching**: hit, ambiguous and miss counts on the user's CR2 set.
- **Table accuracy** against `lfModifier`, for 64, 128 and 256 nodes, at the
  worst corner of the strongest real profile the user's lenses have.
- **The Lens pass**: CPU time at 24 MP with 8 threads per kernel and per
  combination; at the preview level and thumbnail size; on the GPU.
- **Sharpness**: slanted-edge MTF50 for lens alone, lens plus a 2° straighten,
  and no lens.
- **Fill scale** over the user's lenses: how much a toggle magnifies.
- **Metadata cost**: `readImageMetadata` with `LensFacts`, and film-strip fill
  time.
- **Toggle latency** and the GUI thread's longest gap.
- **Ladder RSS** with lens on.
- **Default renders**: digests identical with lens off.

## Open questions for the user (1 answered 2026-10-18)

1. **How much to record.** Answered (user, 2026-10-18): the lens identity,
   crop factor and database stamp only (option A). A revised calibration gives
   a notice, and the pixels can change; the calibration is not stored.
2. **Your lenses and files.** Which lenses do you use on the 80D, including
   any Sigma or Tamron? Please provide one CR2 per lens (zooms at both ends),
   ideally shot RAW+JPEG with the camera's lens aberration corrections on.
   They stay out of git. Optional (user, 2026-10-18): lensfun itself is not
   tested, only our matching, mapping and stage against the synthetic
   database; real files serve one hand-check that the user's lenses match.
