# Spatial global adjustments add a preview context pass

CLAUDE.md and DESIGN.md state the preview invariant: *all adjustments travel in
one uniform block, no CPU image processing, one draw*. Texture, Clarity, and
Dehaze are spatial global controls, which mathematically need **neighbourhood
context** — they cannot be computed correctly from each pixel in isolation. To
preview them live we add an intermediate context pass feeding the main fragment
shader, deliberately retiring the single-pass invariant for this part of the
preview path.

This is the same fork the Sharpen slider faced. Sharpen was made **export-only**
because it is fine, high-frequency detail that a quarter-res preview misrepresents.
Texture, Clarity, and Dehaze are different: they are "tune by eye until the photo
reads right" controls, which are poor to set blind. So they get a live preview
where Sharpen did not.

Dehaze deliberately starts as a **practical raw-editor approximation**, not a
full atmospheric-scattering estimator. The physical model
`I(x) = J(x)t(x) + A(1 - t(x))` would require estimating atmospheric light `A`,
per-pixel transmission `t(x)`, and edge-aware filtering. That is a larger and more
failure-prone analysis pipeline than this first implementation needs. The chosen
implementation uses the same spatial-preview infrastructure to approximate the
expected editing behaviour: reduce or add veiling light, change local luminance
contrast, and apply restrained chroma compensation.

## Considered Options

- **Export-only, keep the preview single-pass.** Consistent with Sharpen and
  cheapest, but you would tune a strong look control without seeing it until
  export. Rejected — bad UX for this control specifically.
- **Single-pass in-shader approximation** (a coarse local-contrast trick with no
  separate blur). Keeps one pass but looks less accurate and, crucially, does not
  generalise. Rejected.
- **Live preview via reusable spatial context.** Chosen: accurate enough,
  tunable, and reusable infrastructure for Texture, Clarity, Dehaze, and a future
  better (previewable) Sharpen.
- **One shared context vs per-control contexts.** Chosen: one shared context
  first. Texture can combine direct small-neighbourhood taps with the source
  image, while Clarity and Dehaze share the blurred luminance/context texture.
  This keeps `RendererCore` smaller and limits render-target churn; dedicated
  radii remain an available refinement if the look proves wrong.
- **Full atmospheric Dehaze estimator.** More physically grounded, but it needs
  atmospheric-light/transmission estimation and edge-aware propagation. Rejected
  for the first implementation in favour of the practical approximation above.

## Consequences

- The preview pipeline gains a notion of an **intermediate render target** (the
  spatial-context result) consumed by `image.frag`. `RendererCore` — still the
  single place the pass(es) are recorded (`0006`) — grows to record the spatial
  pass before the main pass; export and histogram callers reuse the same path at
  their resolutions.
- The "one uniform block, one draw" description in CLAUDE.md/DESIGN.md is now a
  simplification true of every adjustment **except** spatial ones (Texture,
  Clarity, and Dehaze now; Sharpen later). The pipeline order list in DESIGN.md
  gains spatial context as an explicit step.
- Vignette and Grain, added in the same milestone, do **not** use this path —
  they remain per-pixel uniforms and preview single-pass.
- Reduced-resolution preview is accepted as faithful enough for these broad
  controls; this is a property of the effect scale, not a general licence for
  spatial effects (Sharpen stays export-only on the same reasoning, inverted).
