# Colour Grading: three-zone hue/saturation toning, colour or Black & White

[[Black & White]] (ADR 0048) deliberately kept its neutral-grey contract total and
deferred "toning (tinting the monochrome result — sepia, split-toning) ... to a
future Color Grading concept." We now build that concept: **[[Colour Grading]]**,
a three-zone (Shadows / Midtones / Highlights) hue+saturation tint that applies to
both colour and Black & White images.

Scope is the tint itself — hue, saturation, and the two controls that shape the
zones (Balance, Blending). A per-zone brightness control is explicitly out of
scope: that is tone, and tone-by-zone already belongs to [[Shadows / Highlights]]
and the [[Tone Curve]].

## What we decided

- **Three zones, not two.** Shadows/Midtones/Highlights rather than classic
  two-way split toning. ADR 0048 already named the deferred concept "Color
  Grading," not "split toning" — the third zone is a cheap addition once the
  zone-weight machinery exists (one more weight curve, no new machinery) and
  avoids a second redesign later to bolt Midtones on.

- **Hue + Saturation per zone, no per-zone Luminance.** Lightroom's Color Grading
  panel bundles a brightness slider into each zone; we deliberately don't. Every
  chroma control in arraw — [[Saturation]], [[Vibrance]], [[HSL]] — holds
  perceived lightness fixed and leaves tone-by-zone to Shadows/Highlights and the
  Tone Curve. Colour Grading follows the same line: it is a colour control, not a
  tone control.

- **Two global controls: Balance and Blending.** Balance shifts the
  shadow↔highlight crossover point — the load-bearing control of the classic
  split-toning craft, without which the split sits at a fixed point that won't
  suit every photo. Blending softens the width of the zone transitions. Both are
  included (full Lightroom-panel parity) rather than deferring Blending, per
  discussion during grilling. Balance's **sign follows Lightroom's**: negative
  hands more of the tonal range to the Shadows zone, positive to the Highlights.
  Since the value is stored in Lightroom's own `crs:ColorGradeBalance`, an
  inverted sign would silently mirror the grade on any imported sidecar.

- **Its own Develop Group, "Colour Grading."** Groups go 10 → 11. Precedent is
  [[HSL]], which already has its own group distinct from [[Colour]] despite both
  being chroma controls — Develop Groups partition by *coherent, copy-together
  unit*, not by "all colour controls." The eight values here (3 zones × hue/sat +
  Balance + Blending) are meaningless apart from each other but independent of
  Colour, HSL, and White Balance.

- **Applies under [[Black & White]] too — this is the feature's reason to
  exist.** Colour Grading cannot sit where [[HSL]]/[[Saturation]]/[[Vibrance]] sit
  in the shader (skipped entirely when the mono branch runs). Instead it runs
  *after* the Colour/B&W branch merges, tinting whichever luminance signal
  results — a coloured image or the B&W Mixer's neutral grey. This redeems ADR
  0048's explicit deferral: sepia and split-toning on a monochrome image are now
  possible.

- **Pipeline placement: right after the Colour/B&W merge, before Spatial Global
  Adjustments.** This matches Lightroom's own panel order (Basic → Tone Curve →
  HSL/Color → Color Grading → Detail → Effects) and arraw's existing chain
  (`... → HSL → saturation → vibrance → **colour grading** → spatial global
  adjustments → local adjustments → vignette → grain → filmic highlights`).
  [[Local Adjustment]]s then layer their masked edits on top of the graded base
  rather than fighting a downstream grade.

- **Tint applied in Oklab.** Consistent with [[Saturation]], [[Vibrance]],
  [[HSL]], and the [[B&W Mixer]] — every chroma control in this codebase is
  Oklab-based so a hue/saturation move holds perceived lightness. A linear-RGB
  tint would be the odd one out and would visibly shift luminance, undermining
  the tone/toning separation above.

- **Zone weights computed from perceptually-encoded luminance.** Same rationale
  [[Basic Tone]] already used for its LUT (`kGamma = 2.2`, indexing by
  `luminance^(1/kGamma)`): raw linear luminance bunches almost everything into a
  tiny bright range. Using the same encoding means "Shadows"/"Highlights" mean
  the same visual region here as in the Shadows/Highlights tone sliders.

- **Global-only — no [[Local Adjustment]] variant.** Matches [[HSL]] and the
  [[B&W Mixer]], the other multi-parameter chroma controls, neither of which has
  a per-mask variant. Eight values × up to 16 masks would be a lot of state and
  UI for a control whose purpose is a global look.

- **Lightroom-compatible persistence: `crs:ColorGrade{Shadow,Midtone,Highlight}
  {Hue,Sat}`, `crs:ColorGradeBlending`, `crs:ColorGradeBalance`.** Follows the
  pattern used wherever a real Lightroom equivalent exists (White Balance, HSL,
  B&W Mixer); arraw only goes native when there is none (e.g. Filmic Highlights).
  The `crs:ColorGrade*Lum` fields are deliberately unmodeled — per [[XMP Property
  Ownership]], arraw only owns the fields it models, so any `*Lum` value written
  by Lightroom must survive an arraw round-trip untouched rather than being
  overwritten or dropped.

- **UI: sliders, no colour wheels.** Nothing in `src/ui/` uses a colour-wheel
  widget; every control, including the 8-band HSL and B&W Mixer, is
  `QSlider`-based. Colour Grading gets a Hue + Saturation slider pair per zone
  plus Balance and Blending sliders — eight sliders total — rather than
  introducing a new custom-painted widget with no precedent.

- **Naming: "Colour Grading" (British), wire fields stay `ColorGrade*`
  (American).** The glossary term and Develop Group name follow arraw's house
  spelling ([[Colour]], [[Colour Label]], Colour Noise Reduction), the same way
  [[Colour Label]] is stored as `xmp:Label` without translating the term to match
  the wire format. The `crs:` field prefix is Adobe's fixed schema and is not a
  naming choice arraw makes.

## Consequences

- The shader gains a Colour Grading stage between the Colour/B&W merge and the
  spatial-globals pass, plus a CPU reference mirror per [[spot-for-algorithms]].
- Groups go 10 → 11; Copy Settings / Develop Preset / the group checklist all
  grow an eleventh row.
- **Colour Grading runs upstream of [[Filmic Highlights]].** A strong Highlights
  zone tint on a very bright pixel can be partially bleached by Filmic
  Highlights' [[Path to White]] before it reaches the display — this mirrors
  Lightroom's own behaviour (Color Grading also precedes its highlight roll-off)
  and is an accepted consequence of the placement decision above, not a defect.
- `XmpSidecar` must preserve any existing `crs:ColorGrade*Lum` values verbatim on
  save, since arraw does not model them.
- `AdjustmentPanel` grows a new panel section (or reuses `AdjustmentPanel`'s
  slider-row pattern) with eight sliders, shown regardless of the Colour/B&W
  treatment toggle — unlike the Colour/HSL panels, which hide under B&W.
