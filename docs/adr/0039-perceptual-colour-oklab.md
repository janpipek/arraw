# Saturation and Vibrance operate in Oklab

Saturation and Vibrance — both the global controls and the per-[[Local
Adjustment]] versions — used to be a straight lerp toward Rec.2020 luminance grey
(`mix(vec3(luma), c, 1 + amount)` in `image.frag`). Because that mixes in the
*linear* working space, pulling colour up or down also drags perceived
**lightness** and skews **hue** — the classic "saturation thins out the colours"
defect called out in issue #22. We now convert the pixel to **Oklab**, scale only
the `a`/`b` chroma axes, and convert back, so colourfulness changes while
perceived lightness and hue hold.

- **Saturation** scales chroma uniformly.
- **Vibrance** scales chroma weighted by `1 − currentChroma`, so already-vivid
  colours move less than muted ones (the gentler, "protect the skin tones" curve).

## Why Oklab

Oklab is a perceptual colour space (Björn Ottosson, 2020) with a near-orthogonal
lightness axis and even hue spacing, computed from a small fixed matrix, a cube
root, and a second matrix — cheap enough to run per pixel on the GPU and simple
enough to own and unit-test. It is the same space CSS Color 4 and most modern
tools adopted for exactly this job.

## Considered alternatives

- **ICtCp** (Dolby) — better hue-linearity at extreme HDR brightness, but it is
  defined around absolute (PQ) luminance and buys nothing for arraw's SDR output
  while costing complexity. Revisit only if/when arraw gains an HDR output path.
- **CIECAM16** (the RawTherapee route) — a full colour-appearance model;
  powerful, but heavy and hard to make predictable and testable. Against the
  "equations we own and can test" stance of ADR 0033.
- **Keep the linear lerp** — that *is* the defect.

## Consequences

- The Oklab transforms live **once** in a CPU module (`src/OkLab.h`), unit-tested
  for round-trip identity and for "the neutral grey axis carries zero chroma";
  the shader mirrors the same matrices. This is the [[spot-for-algorithms]]
  contract the golden-image tests already enforce for tone.
- Oklab is canonically defined from CIE XYZ (D65). Because arraw's [[Working
  color space]] is linear Rec.2020, the Rec.2020→XYZ step is **folded into**
  Oklab's first matrix, giving a single Rec.2020→LMS matrix (and its inverse).
  Tests pin the folded matrices against a reference grey and a saturated primary.
- **Contrast is deliberately *not* moved.** Issue #22 lumps contrast in with
  saturation, but arraw's Contrast already runs in perceptual log-odds luminance
  and scales RGB proportionally (ADR 0033), so it does not thin colour. Only the
  two chroma controls change.
- **HSL stays as-is.** The 8-band HSL mix is an HSV-space hue/sat/lum tool, a
  distinct concept with its own per-hue masks; it is out of scope here.
- Existing sidecars with non-zero Saturation/Vibrance render differently after
  this change — as with ADR 0033, the field names and slider values are unchanged
  but their interpretation is improved. No version flag or migration.
