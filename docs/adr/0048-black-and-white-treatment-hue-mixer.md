# Black & White: a treatment with a hue-aware mixer, not a desaturation

Until now the only path to a monochrome image in arraw was pulling [[Saturation]]
to −100, which scales Oklab chroma uniformly to zero: a *flat, hue-blind*
greyscale where a blue sky and a red rose of equal luminance collapse to the
identical grey. That is the one thing photographers doing black-and-white work do
*not* want — the whole craft is choosing how each colour translates to a tone (a
red filter darkens a blue sky; a green filter lightens foliage). We add a proper
**[[Black & White]]** treatment: a mode toggle plus a **[[B&W Mixer]]** of eight
per-hue-band weights that decide each original hue's grey value.

Scope is the conversion and the mixer only. **Toning** (tinting the monochrome
result — sepia, split-toning) is deferred to a future Color Grading concept, and
the monochrome result staying strictly neutral grey is part of *this* decision's
contract. **Auto-mix** (analysing the image to seed a flattering default, as
Lightroom does on `V`) is out of scope; conversion defaults to a flat mix.

## What we decided

- **An 8-band hue mixer, not a 3-channel mixer.** Each band (Red, Orange, Yellow,
  Green, Aqua, Blue, Purple, Magenta — the *same* eight centres as [[HSL]],
  reusing `kHslCenters`) carries a −100..+100 weight that darkens or lightens the
  greys made from pixels of that hue. The weighting is **hue-driven and
  saturation-scaled**: a neutral pixel is never moved, a fully saturated one moves
  the full band amount, and all-zero weights reproduce the standard Rec.2020
  luminance (`kLuma`) we already compute. We rejected the classic 3-channel
  Photoshop channel-mixer (`wR·R + wG·G + wB·B`): it cannot darken cyan without
  dragging blue and green, and it does not match the band model the user already
  knows from HSL.

- **Its own Develop Group, "Black & White."** The treatment toggle *and* the eight
  weights live together as one indivisible [[Develop Group]] (groups go 9 → 10).
  They are meaningless apart, so binding them means every Copy/Paste/Preset of
  Black & White carries a *coherent* state — you cannot paste a mix that silently
  does nothing because the target is still colour, nor lose mono-ness by pasting a
  neighbouring group. We rejected folding the toggle into HSL (Lightroom's
  panel-swap model): arraw's groups are about *copy granularity*, not panels, and a
  toggle that changes what a group *means* is exactly the hidden coupling the group
  model exists to avoid.

- **The conversion runs immediately after [[White Balance]], replacing the colour
  stage.** When mono is on: apply WB, collapse to grey via the mixer, then skip
  `HSL → saturation → vibrance` (no-ops on an achromatic signal). Placing it after
  WB means the mix **responds to Temperature/Tint** — the digital analogue of
  screwing a filter onto a colour-balanced scene. Everything downstream (spatial
  globals, local adjustments, vignette, grain, filmic highlights) develops on the
  grey signal.

- **Monochrome means monochrome — local colour is suppressed.** A [[Local
  Adjustment]]'s `temperature`/`tint`/`saturation`/`vibrance` deltas are forced
  neutral while mono is on, in both the CPU model and the shader's LA loop; only
  the tonal deltas act. This keeps the neutral-grey contract *total* rather than
  "neutral except where a mask carries a stray tint," and leaves the door open to
  design real toning later without a half-baked version leaking in now.

- **Lightroom-compatible persistence.** Stored as `crs:ConvertToGrayscale` (the
  toggle) and `crs:GrayMixerRed/Orange/Yellow/Green/Aqua/Blue/Purple/Magenta` (the
  weights, internal −100..100 scale), independent of the HSL `crs:` fields and
  inside the [[XMP Property Ownership]] rule. The weights persist **independently
  of the toggle**, so switching Colour ↔ B&W and back preserves the dialled-in mix.

- **UI: a segmented Treatment switch (Colour | B&W) at the top of the develop
  stack, beside White Balance.** WB never hides, so the toggle has a stable home
  and cannot hide itself; WB also feeds the mix, so the causal grouping is honest.
  Turning B&W on hides the Colour and HSL panels (their controls are inert) and
  reveals the B&W Mix panel, which reuses the existing HSL 8-band slider widget.

## Consequences

- The shader gains a mono branch after white balance and a `convertToGrayscale`
  uniform plus eight mixer weights; per [[spot-for-algorithms]] the CPU model is
  the tested source of truth and the shader mirrors it line-for-line.
- `Saturation −100` and Black & White remain *distinct* code paths and distinct
  glossary terms; the former is a flat desaturation, the latter a hue-aware
  conversion. We accept that both exist.
- Copy Settings / Develop Preset / the group checklist all grow a tenth row.
