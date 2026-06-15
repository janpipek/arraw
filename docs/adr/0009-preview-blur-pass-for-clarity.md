# Clarity adds a blur pass; the preview is no longer strictly single-pass

CLAUDE.md and DESIGN.md state the preview invariant: *all adjustments travel in
one uniform block, no CPU image processing, one draw*. Clarity/Texture is a
local-contrast control, which mathematically needs a **blurred copy of the
image** — it cannot be computed from each pixel in isolation. To preview Clarity
live we add a **second render pass (a blur) feeding the main fragment shader**,
deliberately retiring the single-pass invariant for the preview path.

This is the same fork the Sharpen slider faced. Sharpen was made **export-only**
because it is fine, high-frequency detail that a quarter-res preview misrepresents.
Clarity is the opposite: a broad, low-frequency effect whose large radius is
faithfully approximated at quarter-res — and it is a "tune by eye until the photo
pops" look, which is poor to set blind. So Clarity gets a live preview where
Sharpen did not.

## Considered Options

- **Export-only, keep the preview single-pass.** Consistent with Sharpen and
  cheapest, but you would tune a strong look control without seeing it until
  export. Rejected — bad UX for this control specifically.
- **Single-pass in-shader approximation** (a coarse local-contrast trick with no
  separate blur). Keeps one pass but looks less accurate and, crucially, does not
  generalise. Rejected.
- **Live preview via a real blur pass.** Chosen: accurate, tunable, and the blur
  pass is reusable infrastructure for the deferred Dehaze and a future better
  (previewable) Sharpen.

## Consequences

- The preview pipeline gains a notion of an **intermediate render target** (the
  blur result) consumed by `image.frag`. `RendererCore` — still the single place
  the pass(es) are recorded (`0006`) — grows to record the blur before the main
  pass; export and histogram callers reuse the same path at their resolutions.
- The "one uniform block, one draw" description in CLAUDE.md/DESIGN.md is now a
  simplification true of every adjustment **except** spatial ones (Clarity now,
  Dehaze/Sharpen later). The pipeline order list in DESIGN.md gains the blur as
  an explicit step.
- Vignette and Grain, added in the same milestone, do **not** use this path —
  they remain per-pixel uniforms and preview single-pass.
- Quarter-res preview is accepted as faithful for Clarity; this is a property of
  the effect's scale, not a general licence for spatial effects (Sharpen stays
  export-only on the same reasoning, inverted).
