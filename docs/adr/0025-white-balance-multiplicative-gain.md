# White balance is a multiplicative, blackbody-derived channel gain

White balance was an **additive** offset in linear Rec.2020: `applyTempShift`
added `±t·0.15` to R/B and `applyTint` added `tint·0.05` to G. Additive offsets
lift a zero channel off zero, so a black pixel `(0,0,0)` at 12000 K
(`t ≈ +1.18`) rendered as `(0.177, 0, 0)` — pure red after clamp. Any pixel with
no light acquired colour, most visibly across the black regions of
high-contrast/false-colour images.

White balance is now a **per-channel multiplicative gain** (the von Kries
diagonal model that every raw processor uses): `c *= (kr, kg, kb)`, with neutral
(5500 K, tint 0) = identity. Black-stays-black is then a free consequence —
`0 × k = 0` — not a special case, and the operation is homogeneous (`f(k·c) =
k·f(c)`), so it scales with brightness rather than painting a fixed colour onto
the shadows.

The gains are **blackbody-derived** so the Kelvin numbers mean something: a
target temperature maps through its Planckian-locus chromaticity → XYZ →
Rec.2020 RGB, normalised so 5500 K = `(1,1,1)`; Tint is the orthogonal
green↔magenta offset. The mapping is computed **once on the CPU per parameter
change** (alongside the existing lcms colour code) and handed to the shader as a
single `vec3` gain uniform — the shader only multiplies. Local-adjustment
white balance reuses the *same* CPU function, mapping its relative −100..100
shift to an effective Kelvin, so there is one gain curve everywhere. The WB
picker inverts that same function (1D search over Kelvin to neutralise the
sampled R/B ratio, tint from the green residual) rather than carrying a second,
hand-synced formula.

## Considered Options

- **Multiplicative blackbody gain, computed CPU-side (chosen).** Physically
  correct, black-safe by construction, and the colour maths become a plain C++
  function that is unit-testable without a GPU (identity at 5500 K, monotonic
  warm/cool, picker round-trip). Costs a uniform-layout change (`Ubuf` +
  `image.vert`/`image.frag` + `fillUbuf`) and a regeneration of the
  `white_balance` golden.
- **Clamp/patch the additive model** so a zero channel can't be lifted. Smallest
  diff and preserves the old slider feel, but it is not a white balance — it
  fakes the symptom, stays asymmetric, and leaves the Kelvin numbers
  meaningless. Rejected.
- **Compute the gain per-pixel in the shader** from the Kelvin uniform (no layout
  change). Rejected: it duplicates colour maths into GLSL, spends per-pixel
  transcendentals on a value that is constant per frame, and is only testable
  through GPU golden renders (which skip headless).
- **Match the old slider strength** when calibrating the new gain. Rejected: the
  old behaviour is the bug; there is nothing correct to anchor to.

## Consequences

- The `Ubuf` carries a `vec3 wbGain` (and the local path reuses spare slots in
  `laTone2`/`laColor`), mirrored across `image.vert`, `image.frag`,
  `RendererCore.h`, and `fillUbuf()` — the cross-file uniform contract from
  docs/adr/0006.
- The `white_balance` golden is regenerated; the `syntheticScene` black texel
  gains an always-on assertion (black survives 12000 K) and the `wbGain`
  function gets pure-CPU unit tests that run headless.
- **A given Kelvin now renders differently than before.** Photos previously
  edited under the additive model will shift when re-opened — the sidecar still
  stores `crs:Temperature` in absolute Kelvin (unchanged), only its
  interpretation changed. We were never bit-identical to Lightroom's Kelvin, so
  no compatibility contract is broken.
- White balance stays applied **late** (after exposure/tone/curve, in
  Rec.2020), not at demosaic on camera-native RGB where a fully accurate Kelvin
  adaptation belongs. This caps how physically exact the Kelvin mapping can be;
  moving it upstream is out of scope for this change.
- `CONTEXT.md` gains a **White Balance** term capturing the black-stays-black
  domain rule.
