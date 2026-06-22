# M-a implementation plan — Lens Corrections (lensfun)

Red-first plan for the first lens-corrections milestone. Architecture and
rationale are in [ADR 0027](adr/0027-lens-corrections-cpu-corrected-negative.md);
vocabulary in [CONTEXT.md](../CONTEXT.md). Scope: **lensfun-first** — three
profile-driven corrections (Distortion, corrective Vignetting, lateral Chromatic
Aberration), sourced from the lensfun database and engine. Embedded/Sony data is
M-b behind the same seam. Axial CA deferred.

Ordering chosen after confirming lensfun fully covers the reference rig (Sony
ILCE-6700 + Sigma 56mm F1.4 DC DN, E mount: distortion `ptlens`, TCA `poly3`,
dense `pa` vignetting). lensfun is already installed on the dev box (0.3.4).

## Guiding constraints

- The correction math is **pure `ImageBuffer → ImageBuffer`** and unit-tested
  headless with no GPU (Catch2 in `tests/`), not via the golden-image path
  (ADR 0005). Jan runs the app for feel.
- One apply path. The seam is a **per-shot resolved `RadialCurve` LUT**; lensfun
  (M-a) and embedded/Sony (M-b) both populate the same LUTs, so logic exists once.
- Apply-once, profile-driven, per-correction toggles. No live sliders.

## The unifying type (build first, no I/O)

The seam is the *resolved* correction — already collapsed to radius-indexed
curves, so the apply step never sees a polynomial, a knot table, or lensfun:

```cpp
struct RadialCurve {                  // gain or displacement vs normalised radius
    std::array<float, 64> lut{};      // 1.0 (gain) or 0 (displacement) = identity
};
struct LensCorrectionModel {
    RadialCurve distortion;           // radial displacement
    RadialCurve tcaR, tcaB;           // per-channel radial scale (green = reference)
    RadialCurve vignette;             // radial gain
    bool hasDistortion = false, hasTCA = false, hasVignetting = false;
    QPointF center{0.5, 0.5};         // optical centre, normalised
    QString lensName;                 // profile identity (for sidecar)
    enum class Source { Lensfun, Embedded } source = Source::Lensfun;
};
```

Toggles (user/sidecar controlled) live on `GlobalAdjustment`, separate from the
model (what the profile provides):

```cpp
bool lensCorrectDistortion = false;
bool lensCorrectVignetting = false;
bool lensCorrectCA = false;
```

## Test-first sequence — pure apply (source-agnostic, no lensfun)

Each step: failing test, then code. These never touch lensfun.

Status: steps 1–6 **done** in `src/LensCorrection.{h,cpp}` + `tests/test_LensCorrection.cpp`
(16 cases / 61 assertions, headless). The seam is a per-shot resolved `RadialCurve`
LUT; every curve is a 1.0-identity multiplier (vignette = gain, distortion/tcaR/tcaB
= radial scale of the centre vector). Distortion + TCA share one bilinear resample
(green carries geometry; red/blue add TCA).

1. ✅ **Identity model is a no-op.** Empty model / all-off toggles returns `buf`.
2. ✅ **Vignetting gain.** Radial gain; centre untouched, corners brightened,
   honours an offset optical centre.
3. ✅ **Distortion warp.** Inverse-map bilinear resample; uniform inward scale
   magnifies to the predicted source column.
4. ◑ **Frame refit — `autoFillZoom` done.** Auto-scale-to-fill: smallest zoom (≥1)
   that keeps every corrected pixel sampling in-bounds (binary search on the binding
   edge), applied automatically when distortion is active. **Remaining at
   integration:** set `LoadResult::defaultCrop` (and use `maxInscribedCrop` for any
   residual non-rectangular region) — deferred to step 11, where the viewport crop
   machinery is in scope.
5. ✅ **TCA.** Per-channel radial scale; red/blue move along the radius, green
   reference unchanged; identity TCA is a no-op.
6. ✅ **Toggles compose.** Each correction gated by both its model flag and toggle.

## lensfun populator (M-a source) — DONE

Implemented in `src/LensfunSource.{h,cpp}` + `tests/test_LensfunSource.cpp` (4 cases /
14 assertions on the fixture DB; a hidden `[.realdb]` test resolves the real rig
against the system DB). Feature-guarded by `ARRAW_HAS_LENSFUN`; without lensfun the
two entry points (`lensfunAvailable`, `resolveLensfunModel`) compile and return
false/`nullopt`.

7. ✅ **Build/link liblensfun.** Optional `pkg_check_modules(lensfun)` in CMake →
   `PkgConfig::LENSFUN` + `ARRAW_HAS_LENSFUN`; `lensfun-devel` is the new Fedora dev
   dep (add to AGENTS.md). Missing lib degrades cleanly, no build break.
8. ✅ **Fixed test DB.** `tests/fixtures/lensfun-mini/mini.xml` — one mount/camera/
   lens with known `ptlens`/`poly3`/`pa`, loaded via `lfDatabase::LoadDirectory`, so
   matching/resolution are deterministic and independent of the system DB.
9. ✅ **Match.** `FindCameras` + `FindLenses` on the EXIF strings; reference rig
   resolves (validated against the real DB: Sigma 56/1.4 on ILCE-6700), no-match /
   bad-path / empty-query all return `nullopt`.
10. ✅ **Resolve → LUT.** `resolveLensfunModel(dbPath, query)` drives `lfModifier`
    (decision made: use lensfun's engine, *not* hand-read coefficients, so its
    aperture/distance interpolation is reused and we keep one apply path). It samples
    along the diagonal to the far corner — where the output pixel at t sits at
    normalised radius t, matching `applyLensCorrection`'s `r = dist/maxR` — and fills:
    distortion (`ApplyGeometryDistortion` → radial scale), TCA (`ApplySubpixelDistortion`
    → red/blue scale vs green), vignetting (`ApplyColorModification` on a unit pixel →
    gain). `Initialize`'s returned flags set the `has*` bits. `scale=1.0` so lensfun
    adds no zoom of its own (our `autoFillZoom` owns framing).

## Wiring into the load flow

11. ✅ **Done.** `RawProcessor::load` builds a `LensQuery` from EXIF and resolves the
    `LensCorrectionModel` (lensfun, system DB), storing it on `LoadResult`. Decision:
    rather than apply in `RawProcessor` before `downsample2x`, the **toggle-gated
    apply lives in `DevelopSession`** (mirroring spots) — it corrects the clean
    preview *and* full-res independently into `correctedPreview/FullResBuffer`, and
    spots now build on the corrected base (`clean → corrected → spotted → GPU`).
    `previewForDisplay`/`fullResForExport` fall back spotted → corrected → clean.
    `autoFillZoom` subsumes the distortion frame change, so `defaultCrop` needs no
    extra handling. Validated headless (`[develop-session][lens]`) and end-to-end on
    the real ARW (`[.realfile]`: resolves the Sigma + correction changes pixels).

    (Original step-11 sketch, kept for context:)
    ~~RawProcessor::load builds the model and applies before `downsample2x`, sets
    `LoadResult::defaultCrop`~~
    from the refit. Clean buffer need not be retained — corrections are apply-once;
    re-derive on reload (revisit only if in-session toggling is requested).
12. **DevelopSession** composes lens correction **before** spots:
    `clean → lens-corrected → spotted → GPU`. Confirm `spottedFullRes` /
    `spottedPreview` derive from the corrected buffer and `displayUVToBufferPixel`
    resolves to corrected-buffer pixels.

## Persistence (XmpSidecar) — DONE

13. ✅ **Done.** The three enable toggles round-trip as `arraw:LensCorrect{Distortion,
    Vignetting,CA}` (arraw-owned namespace), written only when on, absent = off on
    load. Decision: store toggles only — the profile *identity* and coefficients are
    re-derived from the file on every load (more robust to DB updates), so no
    `crs:LensProfile*` is persisted in M-a (Lightroom lens-profile interop, a
    different model, is deferred). Round-trip + absent-loads-off tests in
    `test_XmpSidecar.cpp`.

## The rename (mechanical, no data migration)

14. ✅ **Done.** Renamed `GlobalAdjustment::vignetteAmount/Midpoint/Feather` →
    `postCropVignette*` across struct, `AdjustmentPanel` (+ UI label "Vignette" →
    "Post-Crop Vignette"), `RendererCore::Ubuf` + `fillUbuf`, the `image.vert` /
    `image.frag` uniform block (all three identical — verified consistent), and
    `XmpSidecar` (sidecar key already `crs:PostCropVignette*`, no migration). Full
    suite green (256). Pure rename + identical std140 layout, so runtime is
    unchanged; a manual GPU smoke run is still advisable per AGENTS.md since golden
    tests skip headless.

    Also added the Lens Corrections enable toggles to `GlobalAdjustment`
    (`lensCorrectDistortion` / `lensCorrectVignetting` / `lensCorrectCA`),
    setting up persistence (step 13) and the apply-time wiring (step 11).

## UI — DONE

15. ✅ **Done.** `AdjustmentPanel` has a **Lens Corrections** section: a profile-name
    label ("No lens profile" when unmatched) + three checkboxes (Distortion,
    Vignetting, Chromatic Aberration), disabled until `setLensProfileName` is given a
    match. Toggling emits `paramsChanged` and commits one undo step. MainWindow feeds
    the resolved name via `DevelopSession::lensProfileName()` in `syncSessionToEditors`.
    Added the ninth **LensCorrections** [[Develop Group]] (`DevelopGroup` enum, key,
    label, `applyGroups`) and its `DevelopPreset` JSON (`distortion`/`vignetting`/`ca`).
    Tests: `[adjustpanel][lens]` (gating + drive), `[developgroup]` group isolation +
    exhaustiveness, full suite 266.

## Packaging (the M-a cost — parallel track, ties to issue #38)

16. Ship liblensfun + a DB snapshot per platform:
    - ✅ **Runtime DB discovery (shared).** `resolveLensfunModel` with an empty path now
      prefers a DB bundled next to the executable (`../share/lensfun/db` for the AppImage,
      `lensfun/db` for a portable tree) and only then falls back to lensfun's system
      discovery — so relocatable builds don't depend on `/usr/share/lensfun`.
    - ✅ **Fedora RPM**: `BuildRequires: pkgconfig(lensfun)`, `Requires: lensfun` (DB
      included), built with `-DARRAW_WITH_LENSFUN=ON` so it can never silently drop the
      feature. The DB resolves via lensfun's system path from the `lensfun` dependency.
    - ✅ **AppImage** (Ubuntu aqt): build with `liblensfun-dev` + `-DARRAW_WITH_LENSFUN=ON`;
      linuxdeploy bundles `liblensfun.so` (+ glib); a CI step stages the `version_1` XML
      into `usr/share/lensfun/db/`; the smoke job asserts both are inside the AppImage.
    - **macOS**: `brew install lensfun`; verify DB path in the bundle. (Pending.)
    - **Windows (vcpkg)**: verify the lensfun port builds; bundle DB; confirm path
      resolution. Riskiest leg — spike early. Detailed checklist + tracking: **issue #53**.

### Windows checklist (issue #53)

The code is already portable — lensfun only resolves a `LensCorrectionModel` of LUTs
in `LensfunSource.cpp`, the apply math is plain C++, and the whole source is optional
via `ARRAW_HAS_LENSFUN`. Two gaps remain:

1. **Runtime DB discovery — the real blocker.** `RawProcessor.cpp` calls
   `resolveLensfunModel(QString(), query)`; the empty path makes `LensfunSource.cpp`
   call `db.Load()`, which relies on lensfun's *system* paths (`/usr/share/lensfun/`
   on Linux). Windows has no such location, so every lens silently returns `nullopt`.
   Fix (one logic change, harmless on Linux): bundle the DB (`data/db/*.xml`) next to
   `arraw.exe` and resolve a path that prefers the bundled dir (via `LoadDirectory`),
   falling back to empty `db.Load()`.
2. **glib link.** lensfun links `glib-2.0`. `vcpkg install lensfun` pulls it
   transitively, so the vcpkg toolchain's `pkg_check_modules(LENSFUN …)` brings it in.
   If the build falls through to the `find_library`/`find_path` fallback in
   `CMakeLists.txt`, that fallback links only `lensfun` → unresolved glib symbols; it
   needs a matching `find` for `glib-2.0` (+ `gobject-2.0`, `intl`).

Verify: `lensfunAvailable()` true and the Sigma 56/1.4 reference shot resolves a
non-null model with the bundled DB (assertion shape mirrors `test_LensfunSource.cpp`).

## Open / spike before coding

- **CMake + liblensfun link spike** (step 7) and whether to use `lfModifier` vs
  reading coefficients (step 10) — settle the apply-path shape first.
- **Windows vcpkg lensfun** viability (step 16) — confirm before committing M-a to
  a release.
- Reference RAW for end-to-end eyeballing: `_A678886.ARW` (ILCE-6700, Sigma 56/1.4).
