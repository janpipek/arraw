# Focus Peaking computes at full resolution, post-adjustment, and only while enabled

[[Focus Peaking]] is a new preview-only overlay, modelled on the existing
[[Clipping Overlay]] (`docs/adr/0009`): display-only, computed in the shader,
forced off for export/histogram readback, toggle state in `QSettings`, never
the sidecar.

## Decision: always full-resolution, but only while the toggle is on

Focus Peaking's entire job is to surface fine, high-frequency edge detail —
the same kind of detail `docs/adr/0011` identified as **misrepresented by a
downsampled preview**, which is why Sharpen is export-only. Peaking can't take
that escape hatch (its value *is* the live, on-screen judgement), so instead
it always measures the `fullRes` buffer, never the downsampled preview
texture, regardless of the current zoom level. Today `fullRes` is uploaded
lazily only when zoom crosses the preview-pixel threshold (`docs/adr/0056`,
`DESIGN.md`); Focus Peaking adds a second, independent trigger for that
upload — enabling the toggle — rather than gating peaking's accuracy on zoom.

To keep this from becoming an always-on cost, the full-res edge-detection
pass only runs while the Focus Peaking toggle is enabled. Toggling it off
stops the computation entirely (mirrors `sensorClipTex`/`clipWarn`: the
overlay's cost is opt-in, not ambient).

### Considered Options

- **Preview-resolution only (reuse the ADR 0011 spatial context pass
  approach).** Cheap and always live, but the preview is a `downsample2x` of
  the source — exactly the misrepresentation ADR 0011 flagged for Sharpen.
  Rejected: would flag resampling artifacts as sharp, or miss real fine
  detail, undermining the tool's purpose.
- **Full-resolution, gated to zoom ≥ the existing `fullRes` upload
  threshold** (disable/hide the toggle below that zoom). Accurate, reuses the
  existing upload trigger unchanged, but forces a UX carve-out ("zoom in to
  use Focus Peaking") for the common case of judging focus across a filmstrip
  at fit-zoom. Rejected in favour of always-full-res.
- **Full-resolution at any zoom, downsampling only the overlay result
  (chosen).** Correct at any zoom; costs a fullRes upload/edge-detect pass
  triggered by the toggle rather than by zoom, but only while enabled.

## Decision: computed post-all-adjustments, not a fixed pre-creative tap

Focus Peaking is computed at the same pipeline point as the Clipping Overlay
— after every develop adjustment, on the final rendered pixels — rather than
frozen at a point before Sharpening/Clarity/Luminance-NR (which all change
apparent edge contrast).

This app has no separate culling screen: [[Culling]] (rating/colour label) is
a `FilmStrip` thumbnail workflow (`CONTEXT.md`), and the only pixel-level
surface is the single `ImageViewport` used for developing. So "check focus
while culling" and "check focus while developing" are the same overlay on the
same surface, not two use cases needing two tap points. At the moment
culling actually happens (freshly imported images, develop params at
defaults), a post-all-adjustments tap and a fixed pre-creative tap produce
the identical result anyway. Post-all-adjustments is a strict superset — it
also gives live feedback on whether the current Sharpening/NR/Clarity choice
still reads as sharp — at no extra pipeline tap or cache-invalidation cost.

### Considered Options

- **Fixed pre-creative-adjustment tap** (after demosaic/WB/basic tone, before
  Sharpening/Clarity/Dehaze/NR). Would answer "was this shot in focus at
  capture" independent of later edits, but needs its own pipeline tap and a
  separate cache-invalidation rule (only crop/orientation/WB dirty it).
  Rejected: no dedicated culling view exists to justify the extra
  complexity; revisit if arraw ever grows a dedicated loupe/compare view.
- **Post-all-adjustments (chosen).** Same tap point as Clipping Overlay/Gamut
  Warning; no new cache-invalidation story; superset of the rejected option
  for the primary (default-params) culling moment.

## Decision: Sobel gradient magnitude on sRGB-encoded (gamma-space) luma

Focus Peaking measures edge strength as a 3×3 Sobel gradient magnitude over
luma taken **after the display transform** (sRGB-encoded), not the linear
Rec.2020 working-space luma. This is the same display-relative reasoning as
`docs/adr/0009`'s clipping-warning choice: Clipping and Focus Peaking should
agree about "what you see" rather than one judging linear values and the
other gamma-encoded ones. Sobel is the operator essentially every prior-art
focus-peaking implementation (in-camera or otherwise) uses — a well
understood, noise-resistant edge response, for about the same shader cost as
a simpler Laplacian/high-pass kernel.

This is the first high-pass/gradient GPU pass in arraw. There was nothing to
reuse: Sharpening's unsharp mask is CPU-side and export-only
(`ExportWorkflow.cpp`, confirming `docs/adr/0011`'s claim), and
`uSpatialContext` (Texture/Clarity/Dehaze) is a *blurred* (low-pass)
luminance texture — the opposite of what edge detection needs.

### Considered Options

- **Linear Rec.2020 luma.** Working-space-native, but would make Focus
  Peaking and Clipping Overlay disagree about which representation of
  "what's on screen" they judge. Rejected for consistency with ADR 0009.
- **Sobel on sRGB-encoded luma (chosen).** Matches ADR 0009's precedent,
  standard operator for this exact feature.
- **Laplacian/high-pass kernel.** Cheaper (fewer taps), but more prone to
  noise false-positives than Sobel. Rejected — Sobel's cost is negligible
  next to the fullRes upload/pass this feature already requires.
- **Local variance over a window.** Smoother response but pricier per pixel
  and not the standard approach for this feature. Rejected.

**Soft-proofing independence.** Like Clipping (`docs/adr/0009`), the Sobel
gradient is computed on `kRec2020ToSRGB * c` — the plain sRGB transform of
the linear working-space colour — not on `outc` (the actual displayed pixel,
which becomes the proof-profile result when soft-proofing is active). So
"is this edge sharp" keeps one fixed meaning regardless of soft-proofing,
the same way "is this pixel clipped" already does. Toggling soft-proofing
changes what you see; it does not change what Focus Peaking judges.

## Decision: user-adjustable sensitivity, not a fixed threshold

Unlike Clipping's threshold (1.0/0.0), which is a physical constant, there is
no universal cutoff for "edge strength" — gradient-magnitude distributions
vary by lens, scene, and image content. A single fixed threshold risks being
useless (nothing lights up) on low-contrast images and overwhelming
(everything lights up) on high-detail ones. Every existing focus-peaking
implementation (Sony/Fuji/Panasonic/Canon) ships at least a Low/Mid/High
sensitivity choice for this reason — this isn't scope creep, it's table
stakes for the feature to work at all.

**Shape of the control**: a discrete Low/Mid/High choice, not a continuous
slider, exposed as an exclusive `QActionGroup` submenu under
`View → Focus Peaking`, mirroring `docs/adr/0056`'s zoom-preset pattern (one
shared list of levels, consumed by the submenu, instead of a one-off widget).
The `View` menu has no slider anywhere today — Clipping's controls are all
plain checkable `QAction`s — and a slider would need a new home this app
doesn't otherwise have for view-only (non-sidecar) settings. Persisted via
`QSettings` alongside the toggle (`view/focusPeaking*`), same as Clipping.

**Left open for change**: the sensitivity levels should be implemented as a
small ordered table (threshold-per-level), analogous to `kZoomPresets`, kept
in one place — not hand-copied into UI and shader code separately — so
adding a level, retuning a threshold, or later swapping to a continuous
control is a localized change, not a rewrite. Do not hard-code the count of
3 anywhere the number of levels would need to change in more than one spot.

### Considered Options

- **Continuous slider (rejected).** Most granular, but no natural home in
  this app's `View` menu idiom; would need new UI infrastructure this
  feature doesn't otherwise justify.
- **Discrete Low/Mid/High submenu (chosen).** Reuses the `QActionGroup`
  submenu idiom already validated by ADR 0056; kept extensible per above.
- **Per-image adaptive (percentile-based) threshold (rejected for now).**
  Removes manual tuning entirely, but needs a readback/histogram-like step
  per frame (echoing the async histogram machinery of ADR 0004/0035), and
  makes "peaking" mean something different per photo — a stranger contract.
  Worth revisiting only if fixed sensitivity levels prove insufficient.

## Decision: overlay colour is yellow, stacks (does not exclude the other overlays), lowest precedence, and is not blinking

**No blinking, v1.** Blinking would be the app's first continuously-repeating
render trigger — every `ImageViewport::update()` today is event-driven,
including the two existing timers (`histoTimer`, `nrTimer`), both
`setSingleShot(true)` debounces that fire once. Focus Peaking is a GPU
fragment-shader pass (like Clipping/Sensor-Clip), not the CPU `QPainter`
overlay layer used for crop grid/mask handles/brush cursor, so blinking it
would mean a periodic `QTimer` re-running the *whole* render pipeline on a
tick, continuously, while enabled — a real (if small) battery/fan-noise cost
this app has so far avoided, for a convention (blinking) that maps to zebra
stripes/exposure on cinema cameras, not to focus peaking specifically (every
prior-art focus-peaking implementation checked — Sony, Fuji, Panasonic,
Canon, Capture One's focus mask — is a static overlay). Deferred as a
possible refinement if a static overlay proves hard to spot in the field.

**Colour is yellow** — distinct from the red (highlight clip, gamut warning),
blue (shadow clip), and magenta (sensor clip) already in use.

**Stacks with the existing overlays; not mutually exclusive.** Clipping,
Gamut Warning, and Sensor Clipping already stack with each other via a fixed
overwrite order (`image.frag` lines 728–743: gamut red → clip red/shadow blue
→ sensor-clip magenta wins over both). Focus Peaking joins that same stack as
an independent toggle rather than a competing "mode" that auto-disables the
others. Reasons: (1) the overlap between "clipped" and "sharp edge" pixels is
naturally near-empty — a blown region has ~no local contrast to peak on, so
stacking costs almost nothing in the common case; (2) a real combined use
case exists (macro/portrait: is the focus plane sharp *and* is the subject
blown, in one glance); (3) mutual exclusivity would require new state-restore
machinery no toggle in this codebase has today — every existing toggle is
independently persisted and only `J` (highlight+shadow together, one
deliberate pair) mutates more than its own state, and even that never reaches
into an unrelated overlay family.

**Precedence: lowest.** Focus Peaking's yellow is applied before (i.e., is
overwritable by) Clipping/Gamut/Sensor-Clip, so a tonal problem wins the rare
case where the pixel sets coincide — tonal/exposure issues are generally more
consequential to see than a sharpness hint.

**Colour is a hardcoded shader literal, same as the other three overlays** —
no uniform/settings plumbing added in v1. It's still written as one clearly
named constant in `image.frag` (not an inline unlabelled literal), so a
future colour-preset list or picker has an obvious single place to start from
— but that's a documentation/naming discipline, not new infrastructure.
"Theoretically configurable" describes intent for a later change, not a v1
requirement to build toward.

### Considered Options

- **Blinking overlay.** Rejected for v1: new continuous-timer infrastructure,
  battery/fan cost, not the convention for this specific feature. Revisit
  only if static colour proves insufficient in practice.
- **Mutually exclusive with Clipping/Gamut/Sensor-Clip.** Rejected: needs new
  state-restore machinery, forecloses a legitimate combined use case, and the
  pixel-set overlap it would be protecting against is already near-empty.
- **Stacking, lowest precedence (chosen).** Extends the existing overwrite
  chain with no new interaction pattern.
- **Hardcoded shader literal for colour**, matching the other three overlays.
  Rejected per explicit intent to leave the colour open to later
  configuration; costs one named constant now instead of a literal.

## Consequences (so far)

- Enabling Focus Peaking must trigger a `fullRes` upload if not already
  resident, independent of zoom — a new upload trigger alongside ADR 0056's
  zoom-driven one.
- The edge-detection pass reruns whenever `fullRes` or any develop parameter
  changes, same invalidation trigger as the main preview pass already has.
- A new Sobel/gradient-magnitude GPU pass is added — the first of its kind in
  this codebase.
- Sensitivity levels live in one shared, ordered table consumed by both the
  `View` submenu and the shader/uniform path, kept extensible by construction.
- Focus Peaking's colour is a hardcoded shader literal, same mechanism as
  the other three overlays — no new uniform/settings plumbing in v1.
- `image.frag`'s overlay overwrite chain gains one more link, at the bottom:
  peaking yellow → gamut red → clip red/shadow blue → sensor-clip magenta.

## Decision: menu placement, no dedicated keybinding

Focus Peaking is exposed only as a `View` menu entry (the on/off toggle plus
the Low/Mid/High submenu), the same way Sensor Clipping is today — Sensor
Clipping has no keybinding at all, only a `View` menu checkbox. `J` stays
scoped to its existing, deliberate pairing (highlight + shadow clip toggled
together); Focus Peaking does not claim a new single-letter shortcut in v1.

### Considered Options

- **New single-letter shortcut** (e.g. an unclaimed letter). Rejected:
  matches no existing precedent for a *third* overlay family — Sensor
  Clipping, the most recently added overlay, has none either — and keeps
  `docs/keybindings.md` unchanged for this feature.
- **`View`-menu-only, no shortcut (chosen).** Consistent with Sensor
  Clipping's precedent.

## Consequence: fixed a latent `pipelineFor` bug along the way

Implementing the source pass surfaced a real, pre-existing bug: `pipelineFor()`
bound any newly-created pipeline to the shared on-screen `srb` class member
for layout purposes, regardless of which caller (`recordPass` vs
`recordPassWith`) triggered the creation. Every existing caller happened to
run only after `bindingsFor()` had already populated `srb`, so the
order-dependency was latent. Focus Peaking's source pass calls
`recordPassWith` directly and can be the first caller to request a given
render-pass-descriptor format, which — before the fix — created a pipeline
with a null resource-binding layout, silently breaking every later draw
using that cached pipeline (including the caller's own final pass). Fixed by
having `pipelineFor` take the caller's actual `bindings` as a parameter
instead of reaching for the class member. See the commit fixing this for the
full trace.

*The code is `src/render/FocusPeaking.h`, `RendererCore::ensureFocusPeakingMask`,
`shaders/peaking_edge.frag`, `ImageViewport::setFocusPeaking`, and
`MainWindow::applyFocusPeaking`; tests are `tests/test_FocusPeaking.cpp` and the
`[gpu][peaking]` cases in `tests/test_GoldenImages.cpp`.*
