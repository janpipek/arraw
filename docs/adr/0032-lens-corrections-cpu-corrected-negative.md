# Lens corrections are a CPU apply-once step that produces a corrected negative

Profile-driven lens corrections — **Distortion**, corrective **Vignetting**, and
lateral **Chromatic Aberration** (TCA) — are applied on the CPU to the decoded
`ImageBuffer`, turning the libraw image into a *corrected negative* that every
later stage (spots, the shader pipeline, local-adjustment masks, histograms,
export) develops on top of. This is the [[Spot]] pattern of ADR 0017 one layer
deeper: the corrections sit *below* spot removal because they are a property of
the capture, not a develop edit.

```
clean decoded buffer → lens-corrected buffer → spotted buffer → GPU shader
```

The three corrections are **apply-once and profile-driven**, not live-adjustable
sliders: each is a per-correction on/off toggle whose coefficients come from a
**lens profile**, never from a value the user drags. That is what makes a CPU
resample-once-and-cache affordable — there is no per-frame re-warp.

## Context

Issue #22 (the photographer's roadmap) ranks lens/CA correction as priority #1,
"physical integrity". A photographer thinks of distortion, vignetting, and
fringing as one button, but they are three different operations:

- **Distortion** (barrel/pincushion) is a geometric **resampling warp** — it
  moves pixels along a radial polynomial `a + b·r² + c·r⁴ + d·r⁶`.
- **Vignetting** is a per-pixel radial **gain** (no pixel movement).
- **Lateral CA (TCA)** is the *same* warp as distortion but with slightly
  different coefficients per colour channel, so it is three radial scales rather
  than one. **Axial CA** (defocus purple/green fringing) is *not* profile-driven
  and is deferred to its own milestone as a heuristic defringe.

Every existing arraw adjustment lives in the fragment shader. A geometric warp
is architecturally novel, so the placement decision is the heart of this ADR.

Two data sources can describe the same correction. They are unified behind one
`LensCorrectionModel` whose seam is a **per-shot resolved `RadialCurve` LUT**
(displacement/gain vs normalised radius), so the apply step is identical
regardless of source — the "logic exists once" constraint:

- **lensfun** (M-a): an external library + database, matched to the file via the
  lens identity in the EXIF. lensfun *resolves* its parametric model (poly3 /
  ptlens / pa) for this shot's focal length, aperture, and focus distance, and we
  sample that resolution into the LUT — using lensfun's interpolation, keeping a
  single apply path. Confirmed to fully cover the reference body+lens (Sony
  ILCE-6700 + Sigma 56mm F1.4 DC DN, E mount). Cost: a third-party dependency to
  ship on all four platforms (ties to issue #38).
- **embedded RAW data** (M-b): DNG `WarpRectilinear`/`GainMap` and maker notes
  (Sony `SR2SubIFD` first). This data is *pre-resolved by the camera* for the exact
  shot, so it interpolates straight into the same LUT with no aperture/distance
  modelling. No new dependency.

M-a ships lensfun-first (proven engine, reference lens fully covered); M-b adds
the embedded/Sony populator behind the same LUT seam. The ordering was chosen
after verifying lensfun coverage; embedded-first would have been preferred had
coverage been unknown or absent.

## Considered options

- **CPU apply-once on the decoded buffer (chosen).** Lens correction logically
  happens in sensor space, before any creative geometry. Baking it into the
  decoded buffer makes that ordering automatic and matches the apply-once,
  profile-driven nature of the feature. Correction runs *before* `downsample2x`,
  so the preview is corrected for free and matches full-res. Reuses the clean-
  buffer / cached-derivative discipline already proven for spots (ADR 0017).
- **In-shader UV remap.** Fold the warp into `image.vert`/`image.frag` alongside
  crop+rotation (ADR 0007), with TCA as three per-channel fetches. This is the
  right answer *only if* corrections are live-adjustable; they are not, so it
  buys live updates nobody needs while weaving warp math through the crop/rotation
  chain and the inverse `displayUVToBufferPixel` mapping. Rejected for v1.
- **Dedicated GPU resampling pre-pass.** A separate corrected-texture pass before
  the main pipeline. Allows fancier resampling kernels but adds a pass, a texture,
  and a preview/full-res divergence risk for no benefit at apply-once cadence.
  Rejected.

## Consequences

- A new `LensCorrection` module in `arraw_core` holds (1) the pure
  `LensCorrectionModel` struct with its resolved `RadialCurve` LUTs, (2) pure
  `ImageBuffer → ImageBuffer` apply functions for distortion, vignetting, and TCA,
  and (3) source populators — a lensfun populator (M-a) and later an embedded/Sony
  extractor (M-b) — that fill the LUTs. The struct and apply functions are
  headless-unit-testable with **no GPU** (preferred over the golden-image path,
  ADR 0005); the lensfun populator is tested against a small fixed fixture DB so
  matching/resolution stays deterministic and independent of the system database.
- Correcting distortion changes the usable frame, so the corrected image is
  **auto-scaled to fill** and `LoadResult::defaultCrop` is set to the largest
  inscribed rectangle, reusing the `maxInscribedCrop` machinery from ADR 0007.
- Spots and local-adjustment masks are now placed on the *corrected* image.
  Spots store corrected-buffer pixel coordinates; masks store normalised
  corrected-image UVs in the oriented frame (ADR 0010). Toggling a correction
  after placing them may shift the subject under the stored geometry; acceptable
  because corrections are an apply-once load-time decision. The [[Spot]]
  vocabulary and viewport coordinate chain must treat the lens-corrected buffer
  as the "original buffer".
- **Vignette is renamed to Post-Crop Vignette.** The existing creative effect
  (ADR 0026) collided in name with corrective vignetting. The creative control
  becomes **Post-Crop Vignette** (matching Lightroom); the new corrective control
  is **Vignetting** under the Lens Corrections group. There is **no sidecar
  migration**: the creative effect is already stored as `crs:PostCropVignette*`
  (`XmpSidecar.cpp`), so the rename is code symbols (`vignetteAmount` →
  `postCropVignetteAmount`), the UI label, ADR 0026's vocabulary, and CONTEXT.md
  only. The corrective control claims Lightroom's lens fields
  `crs:VignetteAmount` / `crs:VignetteMidpoint`.
- **Lens Corrections becomes a new Develop Group** (the ninth), holding
  Distortion, Vignetting, and Chromatic Aberration. Per ADR 0023/0026 group
  rules it travels via Copy Settings / Develop Presets.
- The sidecar stores **per-correction enable toggles plus profile identity**
  (lens name, source = `embedded` | `lensfun`); coefficients are re-derived from
  the file or lensfun on every load, never persisted. Enables Lightroom-style
  round-trip (`crs:LensProfileEnable`, `crs:LensProfileName`).
- When no profile is found, all corrections are a silent no-op; the UI reflects
  "no profile available" rather than failing the load.
