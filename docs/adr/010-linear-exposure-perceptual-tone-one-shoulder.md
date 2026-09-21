# Exposure is linear, tone shaping is perceptual, and one shoulder ends the chain

This ADR is written ahead of the code. Exposure exists (ADR 007); nothing else
in Tone does, which is why this is cheap to settle now.

`main` applies Exposure as a shift in log-odds on gamma-encoded luminance:
`k = 2^EV; x = kx / (1 - x + kx)` (`src/develop/BasicTone.cpp:23`). Its ADR 0033
says plainly that "the EV label means perceptual, stop-like exposure; it is not
a literal multiply". The shape anchors 0 and 1, so Exposure there can never
clip — it has a small display transform folded inside it.

darktable splits the same job in two. Its `exposure` module is a linear gain in
scene-referred camera RGB, "a color-safe brightening similar to increasing ISO";
the S-curve lives in `sigmoid` or `filmic rgb` at the end, as "a modified
generalized log-logistic curve" pivoting on middle grey. Its manual is explicit
in both directions: set exposure first, and never run two display transforms at
once.

## Decision

**Exposure is a scene-linear multiply**, `2^EV` per channel, as implemented. It
is what the word means, it is exactly invertible, it is a scalar that composes
into the camera matrix at no per-pixel cost, and ADR 003's float working space
means values above 1 survive rather than clip.

**Tone shaping is perceptual.** Contrast, Highlights, Shadows, Whites, Blacks
and the Tone Curve act on `x = max(y, 0)^(1/2.2)`, carrying forward `main`'s
model for reasons that outlive it:

- `crs:ToneCurvePV2012` points are interpreted in gamma space, so a linear
  reading renders the same sidecar differently (`main` ADR 0003).
- A curve widget needs a perceptual x-axis; linear 0.25 is upper-midtone grey,
  not the dark quarter.
- Regional masks are meaningless in linear, where the shadows are a sliver
  near zero (`main` ADR 0033).
- A LUT indexed linearly spends its resolution on highlights and collapses the
  shadows into a column or two — which bites the GPU path, not the CPU one.

**Colour operations are Oklab**, unchanged from `main`'s ADR 0039: scaling
chroma in a linear space drags lightness and skews hue, and Vibrance needs a
perceptual chroma to weight against.

**Exactly one display transform, always on, at the end.** The shoulder that
rolls highlights toward white is a mandatory stage of the output transform with
an adjustable amount, not an effect a photographer switches on. The feature
brief calls it Filmic Highlights and words it as an extra; that wording is
superseded here.

What is mandatory is the stage, not a nonzero amount. The control has a true
neutral, and a photographer who asks for none gets a hard clip — what this
forbids is the shoulder arriving later as an optional effect, leaving every
render before it to clip for want of one (ADR 013). Its default is a gentle
roll, so an export clips only when asked to.

## Consequences

- **A photograph at +1 EV will not match `main`.** Brighter highlights, less
  lifted shadows. At 18% grey the two agree exactly; by 0.6 linear `main` gives
  0.76 where this gives 1.2.
- **Exports clip without the shoulder**, so it cannot be deferred past the
  first tone control — Contrast pushes values above white by design (ADR 013).
- **Histograms live in the curve's space**, or the graph disagrees with the
  picture (`main` ADR 0003).
- **Two encodings coexist deliberately**: scene-linear for Exposure, white
  balance, resampling and blending; perceptual for tone shaping and colour.
  Every stage states which it is in, the way every stage states its reference
  frame in ADR 009.
- **Values above 1 stay live** until the output transform, which is the only
  place clipping is allowed to happen.
