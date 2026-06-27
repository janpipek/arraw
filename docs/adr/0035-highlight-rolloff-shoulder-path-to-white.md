# Highlight Roll-off: a shoulder with a path to white, last in the develop chain

ADR 0031 gave Exposure a log-odds sigmoid that compresses *that control* toward
white, and made working values above 1 [[Recoverable Headroom]] that stay live
until a final boundary. But the final boundary for preview is still a hard
`clamp()` in the display transform, and any control after Exposure (Whites,
HSL Luminance, a masked exposure boost) can push values back above 1 to hit that
cliff. The result is the "digital" highlight clipping and hue skew issue #22
asks us to soften. We add **Highlight Roll-off**: an opt-in develop control that
rolls the upper range gracefully into display range and lets saturated highlights
fade to white instead of clipping or going neon.

## What it does

A single control, **Highlight Roll-off** (0..100, default 0). At 0 the pipeline
is byte-for-byte today's behaviour. Above 0 it, in one stage:

1. **Shoulder** — maps luminance through a sigmoid that leaves shadows and
   midtones essentially untouched and bends the approach to white, compressing
   headroom (and values near 1) smoothly into range. RGB is scaled by the
   luminance ratio, so hue is preserved by the compression itself.
2. **Path to white** — desaturates toward white as luminance climbs into the
   shoulder, reusing the Oklab chroma machinery from ADR 0034. This is what stops
   a bright saturated red from skewing to orange (per-channel clipping) or staying
   an unphotographic neon (luminance-only roll-off).

## Placement: last in the *shared* chain, not in the display transform

The roll-off runs as the final develop step — after grain, **before** the
`displayEncode` fork that splits preview (sRGB encode) from export (linear
readback → CPU lcms output transform). This matters:

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
- **Always-on baked shoulder** — rejected: it would silently re-render every
  existing edit, a second compatibility break after ADR 0031. Default 0 keeps old
  sidecars stable and makes the look opt-in.
- **Two controls (roll-off + separate path-to-white amount)** — deferred. v1
  couples desaturation to the roll-off amount for one predictable knob; splitting
  them is an additive extension if photographers want neon highlights back.

## Consequences

- The shoulder is a scalar luminance map, unit-tested as a pure function
  (anchors 0; monotonic; ≤1 for all finite input; identity at amount 0). It is
  *not* folded into the Basic Tone LUT, which is applied first and cannot see the
  final value. Path to white reuses the tested `src/OkLab.h` module (ADR 0034).
- New uniform field `highlightRolloff`. Per the standing gotcha it must be added
  to **all three** std140 declarations — `image.vert`, `image.frag`, and `Ubuf`
  in `RendererCore.h` — in the same change, even though only the fragment stage
  reads it, or the GL backend fails to link and the viewport renders black.
- It is a member of the **Tone** [[Develop Group]], so it travels with the other
  tone controls in Copy Settings and Develop Presets. Stored in arraw's own
  namespace (`arraw:HighlightRolloff`, default 0) — there is no Lightroom-
  compatible equivalent, so unlike the `crs:` tone fields it is not round-tripped.
- The roll-off bakes into exported pixels (it is in the shared chain). A future
  HDR output path would want to skip or re-target it; the control is structured
  so that path can gate it later, the same way `displayEncode` already gates the
  SDR display transform.
