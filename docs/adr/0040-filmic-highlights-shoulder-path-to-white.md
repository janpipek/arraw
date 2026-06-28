# Filmic Highlights: a shoulder with a path to white, last in the develop chain

ADR 0033 gave Exposure a log-odds sigmoid that compresses *that control* toward
white, and made working values above 1 [[Recoverable Headroom]] that stay live
until a final boundary. But the final boundary for preview is still a hard
`clamp()` in the display transform, and any control after Exposure (Whites,
HSL Luminance, a masked exposure boost) can push values back above 1 to hit that
cliff. The result is the "digital" highlight clipping and hue skew issue #22
asks us to soften. We add **Filmic Highlights**: a develop control that rolls the
upper range gracefully into display range and lets saturated highlights fade to
white instead of clipping or going neon.

## What it does

A single control, **Filmic Highlights** (0..100, **default 25**). At 0 the
pipeline is byte-for-byte the old hard-clip behaviour. Above 0 it, in one stage:

1. **Shoulder** — maps luminance through a sigmoid that leaves shadows and
   midtones essentially untouched and bends the approach to white, compressing
   headroom (and values near 1) smoothly into range. RGB is scaled by the
   luminance ratio, so hue is preserved by the compression itself.
2. **Path to white** — desaturates toward white as luminance climbs into the
   shoulder, reusing the Oklab chroma machinery from ADR 0039. This is what stops
   a bright saturated red from skewing to orange (per-channel clipping) or staying
   an unphotographic neon (luminance-only roll-off).

## Why on by default (and why 25)

Every mainstream raw developer ships a graceful highlight roll-off as part of its
*default* rendering — Lightroom and Capture One bake a shoulder into the base
profile/film curve; darktable's scene-referred modules (filmic/sigmoid) are on by
default. A hard digital clip is the look almost no editor ships and the one issue
#22 calls a defect. So the default is **on**, at a gentle 25: the knee sits at
luminance ≈ 0.875, so only the brightest ~12% of the range plus super-white
headroom is eased — visible grace on blown highlights, no noticeable flattening of
midtones. The control remains fully tunable; double-click resets to 25, and 0
restores the hard clip for anyone who wants it.

This reverses the opt-in default of the first draft of this ADR. The compatibility
cost (re-rendering existing edits) was acceptable here because the feature had not
shipped — no sidecars in the wild carry the attribute yet — and matching user
expectation outweighs holding a default no editor uses.

## Placement: last in the *shared* chain, not in the display transform

It runs as the final develop step — after grain, **before** the `displayEncode`
fork that splits preview (sRGB encode) from export (linear readback → CPU lcms
output transform). This matters:

- Putting it in the display transform (the obvious "replace the clamp" instinct)
  would touch **preview only**; export skips that branch, so the exported file
  would still hard-clip at the 8/16-bit encode and diverge from the preview. The
  shared-chain placement keeps one source of truth and preserves WYSIWYG.
- It is placed **last**, not "before the user curve" as issue #22 literally
  suggests. arraw is not a scene-referred pipeline; the defect is the end-of-chain
  clipping cliff, and only an end-of-chain stage catches headroom produced by
  *every* upstream control, including Local Adjustments.

## Considered alternatives

- **Per-channel filmic sigmoid (ACES-style)** — rejected: clipping channels
  independently *causes* the hue skews we are removing (the notorious blue→cyan,
  red→orange). We use a luminance sigmoid plus an explicit path to white (the
  AgX / darktable-`sigmoid` approach).
- **Opt-in, default 0** — the first draft's choice, reversed (see above): it left
  arraw's default look as the hard digital clip the feature was meant to cure.
- **A plain on/off toggle (one fixed shoulder)** — rejected: a single slider is
  barely more complex and lets photographers tune how filmic the highlights read
  per image, which different scenes genuinely want.
- **Two controls (shoulder + separate path-to-white amount)** — deferred. v1
  couples desaturation to the one amount for a predictable knob; splitting them is
  an additive extension if photographers want neon highlights back.

## Naming

Shipped as **Filmic Highlights** rather than "Highlight Roll-off" or "Highlight
Compression": the film metaphor is the only name whose own meaning carries *both*
halves of the effect — the tonal shoulder *and* the colour bleaching toward white
— whereas "compression" names only the tonal half and "roll-off" is signal jargon
a casual user won't parse. RawTherapee's "Highlight Compression" was the closest
prior art considered.

## Consequences

- The shoulder is a scalar luminance map, unit-tested as a pure function
  (anchors 0; monotonic; ≤1 for all finite input; identity at amount 0). It is
  *not* folded into the Basic Tone LUT, which is applied first and cannot see the
  final value. Path to white reuses the tested `src/OkLab.h` module (ADR 0039).
- New uniform field `filmicHighlights`. Per the standing gotcha it must be added
  to **all three** std140 declarations — `image.vert`, `image.frag`, and `Ubuf`
  in `RendererCore.h` — in the same change, even though only the fragment stage
  reads it, or the GL backend fails to link and the viewport renders black.
- It is a member of the **Tone** [[Develop Group]], so it travels with the other
  tone controls in Copy Settings and Develop Presets. Stored in arraw's own
  namespace (`arraw:FilmicHighlights`) — there is no Lightroom-compatible
  equivalent, so unlike the `crs:` tone fields it is not round-tripped. It is
  written **unconditionally** (not skip-when-zero): with a non-zero default, an
  explicit 0 must persist, or it would read back as the default and silently
  re-enable itself.
- It bakes into exported pixels (it is in the shared chain). A future HDR output
  path would want to skip or re-target it; the control is structured so that path
  can gate it later, the same way `displayEncode` already gates the SDR display
  transform.
