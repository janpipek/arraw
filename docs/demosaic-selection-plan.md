# Implementation plan — Demosaic Algorithm selection

Red-first plan for the Demosaic Algorithm milestone (issue #22, "Demosaicing
Selection"). Architecture and rationale are in
[ADR 0033](adr/0033-demosaic-selection-redecode-via-load-path.md); vocabulary in
[CONTEXT.md](../CONTEXT.md) under **Demosaic Algorithm**.

Scope: expose libraw's **LGPL built-in** demosaic set as a per-image, undo-able
develop choice in the **Detail** group, persisted as `arraw:DemosaicAlgorithm`,
applied by re-decoding through the existing load path. AMaZE/RCD and an X-Trans
Markesteijn menu are explicitly **out of scope** (deferred — see ADR).

## Guiding constraints

- **AHD stays the default.** Absent/unknown token → AHD → today's exact decode,
  so the golden-image suite (ADR 0005) and existing sidecars do not move.
- **The pure core is unit-tested headless** (Catch2 in `tests/`), no GPU, no RAW
  file: the token↔`user_qual` mapping, the cache-key composition, and the XMP
  round-trip. The visual difference between algorithms is judged by running the
  app, not in a golden test.
- **One re-decode path.** A demosaic change goes through the same async load
  machinery as opening an image — no second decode mechanism.
- **Honesty on non-Bayer.** The menu is shown disabled (not substituted) on
  X-Trans/Foveon/non-RAW (`imgdata.idata.filters`).

## The type (build first, no I/O)

A small enum plus its libraw mapping and token, all in one place — the single
source of truth the sidecar, the cache key, and `RawProcessor` all read from:

```cpp
enum class DemosaicAlgorithm { AHD, VNG, PPG, DCB, DHT, AAHD, Linear };

// AHD is the default everywhere.
constexpr DemosaicAlgorithm kDefaultDemosaic = DemosaicAlgorithm::AHD;

int librawUserQual(DemosaicAlgorithm);          // AHD->3, VNG->1, ... Linear->0
QString demosaicToken(DemosaicAlgorithm);       // "AHD", "VNG", ...
DemosaicAlgorithm demosaicFromToken(QString);   // unknown/empty -> kDefaultDemosaic
```

`demosaicAlgorithm` lives on `GlobalAdjustment` (Detail group), defaulted to
`kDefaultDemosaic`, next to `sharpening` / `colorNoiseReduction`.

## Test-first sequence — pure core (no Qt UI, no libraw)

Each step: failing Catch2 test, then code. New file
`src/DemosaicAlgorithm.{h,cpp}` + `tests/test_DemosaicAlgorithm.cpp`.

1. `librawUserQual` maps each of the 7 values to the documented `user_qual`
   integer (AHD→3, VNG→1, PPG→2, DCB→4, DHT→11, AAHD→12, Linear→0).
2. `demosaicToken` / `demosaicFromToken` round-trip every enumerator.
3. `demosaicFromToken` returns AHD for `""`, for an unknown token (`"AMaZE"`),
   and for garbage — the silent-fallback contract.
4. Cache-key helper: `decodeCacheKey(path, algo)` appends `|<token>` and differs
   per algorithm but is stable for the same `(path, algo)`. (Extend the existing
   `decodeCacheKey` in `ImageLoadWorkflow`.)

## Test-first sequence — persistence

`tests/test_XmpSidecar.cpp` (existing).

5. Writing a `GlobalAdjustment` with a non-default algorithm emits
   `arraw:DemosaicAlgorithm` with the token; reading it back yields the enum.
6. A sidecar **without** the property resolves to AHD (legacy compatibility).
7. The Detail group round-trips it through Copy/Paste and Develop Preset
   (`tests/` for `DevelopPreset` / group selection) — confirm it is included in
   `DevelopGroup::Detail` and reset-to-default returns AHD.

## Wiring — decode (libraw)

8. `RawProcessor::load` and `decodeImage` gain a `DemosaicAlgorithm` parameter;
   `RawProcessor` sets `imgdata.params.user_qual = librawUserQual(algo)` before
   `dcraw_process()` (RawProcessor.cpp ~line 137, beside the other params).
   Default argument = `kDefaultDemosaic` so existing call sites compile unchanged
   until updated.
9. Resolve the token from the sidecar **before** dispatching the decode (reuse
   the up-front `pendingPreviewParams` resolution) and pass it to `decodeImage`.
10. Extend `decodeCacheKey` call sites (load, store, **batch export**, headless
    CLI at MainWindow.cpp ~1784) to include the algorithm — so export honours the
    per-image choice and matches the preview.

## Wiring — UI + re-decode

11. Add a "Demosaic" subheader + `QComboBox` at the **top of the Detail group**
    in `AdjustmentPanel` (above Sharpen), populated soft→sharp with tooltips per
    entry; selection emits a develop-edit signal.
12. The change handler pushes an undo command (writes `demosaicAlgorithm`, marks
    dirty) and **dispatches a re-decode** via the load path, keeping the current
    image on screen until the new decode swaps in. A cache hit (algorithm already
    tried, incl. undo/redo) is instant.
13. Disable the combo with an explanatory tooltip when the open image is X-Trans
    (`filters == 9`), Foveon/demosaiced (`filters == 0`), or a standard image —
    surface `filters` from the decode (or a cheap header peek) on `LoadResult`.
14. Some busy affordance during a first-time (cache-miss) re-decode — reuse
    whatever the initial-load path shows.

## Out of scope (deferred — do not build)

- AMaZE (GPL3 demosaic-pack) and RCD (not in libraw) — would need a custom
  overlay port / licence change. Revisit only if the built-in set proves weak.
- An X-Trans **Markesteijn 1-pass/3-pass** menu — separate sensor-specific design.
- A global "default algorithm for new images" preference — v1 is per-image only.
