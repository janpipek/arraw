# Luminance Noise Reduction unifies the NR pre-pass around an edge-aware luma filter

ADR `0034` shipped **Colour Noise Reduction** as a cached quarter-res GPU chroma
pre-pass and explicitly deferred luminance NR: *"it wants full-res to judge and
destroys fine detail — a different problem."* This milestone ships the luminance
half and, in doing so, **refactors the two reducers into one unified Noise
Reduction subsystem** behind a swappable filter strategy.

## The dual construction

Colour NR decomposes each pixel into luma `Y = dot(c, kLuma)` and a unit-luma
chroma ratio `r = c / Y`, then **smooths `r` and preserves `Y` exactly**.
Luminance NR is its exact **dual**: same decomposition, then **smooths `Y` and
preserves `r` exactly** (`c' = Y' · r`). The two stages are therefore *orthogonal
by construction* — luma NR touches only luminance, chroma NR touches only colour,
neither contaminates the other, and the order between them does not change the
result. This is why a unified subsystem is not just a packaging convenience: the
decomposition and recombination are genuinely shared, and SPOT (logic exists once)
is served by expressing them once.

## Why unify rather than bolt a sibling on

The cheaper option was to leave `0034`'s four chroma passes byte-for-byte and
chain a new luma stage before them. We rejected it: the recombine math, the
`Y`/`r` decomposition, the per-slot cache, and the debounce are all common, and
two parallel copies would drift. The unified chain (per slot, cached) is:

1. extract chroma ratio `r` → ¼-res (`nr_extract.frag`, unchanged).
2. blur H, 3. blur V over `r` → ¼-res (the separable Gaussian, unchanged).
4. extract luma `Y` → full-res (this *is* the existing `spatial_extract.frag`).
5. bilateral H, 6. bilateral V over `Y` → full-res (**the swappable strategy**).
7. recombine → full-res:
   `c'' = mix(Y, Y', amount) · mix(c/Y, r', strength)`.

The luma legs (4–6) are skipped when **Amount == 0**; the chroma legs (1–3) when
**Strength == 0 or Smoothness == 0** (the `0034` rule). With both off the whole
pre-pass is skipped and the main pass samples the raw slot, exactly as before — so
the refactor is an **exact no-op for existing chroma-only edits**, which the golden
images and the skip predicate guard.

**Accepted cost:** this rewrites shipped, green chroma shader/renderer code and
re-validates it. Justified by the shared decomposition and the strategy seam below.

## The filter is a swappable strategy; v1 is an edge-aware bilateral

A plain Gaussian (what chroma uses on `r`) would smooth the very signal that
carries detail. v1 uses a **separable bilateral**: a spatial Gaussian whose taps
are down-weighted when their luma differs too much from the centre, so edges
survive and flat noise smooths. But the algorithm sits behind a **strategy seam** —
"given the raw/`Y` texture, record passes that produce a denoised full-res `Y'`" —
so a guided filter, wavelet shrinkage, or non-local means can replace passes 4–6
later, and the choice can become user-facing the way Demosaic Algorithm is
(`0036`) without touching the rest of the chain. We do **not** hardcode the bilateral
the way `0034` hardcoded its four passes.

Two design points that are hard to reverse and therefore fixed here:

- **Edge metric in perceptual space.** The range (edge-stop) term measures luma
  differences in the `tone::kGamma`-encoded domain (`BasicTone.h`), the same
  perceptual encoding the Tone Curve and Curve Input Histogram already use — *not*
  linear Rec.2020 `Y`. Luminance noise lives in the shadows, where linear light
  compresses differences; a linear edge-stop would over-smooth shadows and under-
  protect highlights, and the Detail knob's effect would drift with exposure.
  Single-sourcing `tone::kGamma` avoids inventing a second perceptual encoding.

- **Native-resolution, judged at 1:1.** Unlike chroma (low-frequency, faithful at
  ¼-res), luminance noise is high-frequency grain the ¼-res reduction would average
  away before filtering. The luma legs run at the active slot's **native** size
  (full-res on FullRes, half-res on Preview), radius calibrated in full-res sensor
  pixels and scaled per slot. The fit-zoom preview is therefore approximate and the
  photographer judges at 1:1 — the standard NR workflow, and the honest reading of
  `0034`'s "wants full-res to judge."

## Controls, persistence, defaults

Two user controls, mirroring Lightroom's two primary luminance knobs:

- **Amount** (`crs:LuminanceSmoothing`, **default 0**) — master strength / blend
  opacity. Default 0 keeps arraw neutral on import (the `0033`/`0034` precedent).
- **Detail** (`crs:LuminanceNoiseReductionDetail`, **default 50**) — the bilateral
  range sigma / edge-protection threshold. Default 50 is Lightroom parity.

Lightroom's third knob, **Contrast**, is deferred. The spatial sigma is a small
fixed internal constant (Amount drives blend, Detail drives the range term), so the
user sees exactly two sliders. Both calibrations live as tested C++ functions in
`core/NoiseReduction.h` (the `0034` single-source-of-truth pattern), the shader
taking scalars from them. Carried in the Detail Develop Group; one undo per change.

UI and domain model gain a parent **Noise Reduction** concept with two halves
(Luminance: Amount + Detail; Colour: Strength + Smoothness), mirroring Lightroom's
panel and the now-unified backend (CONTEXT.md).

## Caching and debounce

The cache key extends to `(lumaAmount, lumaDetail, chromaStrength,
chromaSmoothness, generation)`. The full-res bilateral is now the most expensive
stage, so it debounces through the existing `nrTimer` (~200 ms after a slider
settles) like the chroma blur; dragging repaints cheaply against the cached
texture. Export and full-res-readback paths bypass the debounce and use committed
values, as in `0034`.

## Consequences

- **No `Ubuf` change.** Like `0034`, NR keeps its own `NrUbuf`/`nrbuf` and its own
  shaders; the main pass's uniform block is untouched, so the three-place uniform
  gotcha does not apply. A field may be added to `NrUbuf` for the range sigma; the
  `static_assert`s move with it.
- **GPU-only spatial result.** The bilateral output is verified visually; golden
  tests skip without a GPU. What *is* headlessly unit-tested: the Amount/Detail
  calibration functions, the XMP round-trip, the skip/activation predicate, and the
  cache-key equality — the test-first surface of this milestone.
- **Orientation.** The new luma legs reuse the `NrUbuf::flipV` mirroring already in
  `nr.vert`, so they stay aligned with the uploaded orientation on Y-up backends.
- **VRAM.** One added full-res RGBA32F intermediate per active slot for `Y'`.
- A separable bilateral approximates a true 2D bilateral and can leave faint cross
  artifacts at very strong settings; accepted for the interactive control. Higher-
  quality strategies (guided/wavelet/NLM) are the documented follow-up, and the seam
  exists for them.
