# The tone controls are a pivot, four windows, and a shoulder

ADR 010 settled where tone shaping happens — on `x = max(y, 0)^(1/2.2)`, with
exposure left behind in scene-linear and one shoulder ending the chain — and
what it does not do is say what Contrast, Highlights, Shadows, Whites and
Blacks are. `main` has shapes for them, but they were built around an exposure
that lived in the same chain and could never exceed white, which ADR 010
replaced; and they reach the pixels through a 256-entry LUT, which is a GPU
optimisation rather than a contract. So the shapes are designed here.

## Decision

**Closed form, not a lookup table.** The chain is evaluated arithmetically per
pixel, with the plan holding the resolved coefficients. It is exact, it has no
resolution to argue about, and it keeps ADR 011's rule that the order lives in
one function. A LUT is the optimisation the GPU will want; when it comes it is
built from this function and compared against it, rather than being a second
definition of the same curve.

**Tone acts on luminance, and RGB follows by ratio.** Each control maps
`y` to `y'`, and the colour is scaled by `y'/y`, so hue and saturation are
exactly preserved and colour work stays where ADR 010 puts it, in Oklab. The
alternative — the same curve per channel — desaturates highlights as a side
effect of a tone control and overlaps with what the shoulder already does
deliberately.

**Contrast pivots on middle grey, in log–log.** `x' = p·(x/p)^s`, with
`p = 0.18^(1/2.2) ≈ 0.459` and `s = 2^(c/200)`, so ±100 is a perceptual slope
of 1.41 or 0.71 at the pivot. Grey is what a photographer expects to hold still
under a contrast control, and a power law about it is monotone everywhere,
never negative, and continues smoothly past white.

Deliberately *not* an S that pins white. Such a curve has zero slope at white,
which is clipping by another name: every value above it flattens into the same
number, and the headroom ADR 010 keeps live would die at the first contrast
control. Letting bright values rise past 1 is only safe because the shoulder is
there to catch them, which is why these two cannot be separated or reordered.

**Shadows and Highlights are windows, added.** A smooth window that is zero at
both ends, so each moves its own region and neither touches black, white or the
other's territory:

| Control | Window (in `x`) | Peak | Amplitude |
|---|---|---|---|
| Shadows | 0 → 0.6 | 0.3 | ±0.12 |
| Highlights | 0.4 → 1.2 | 0.75 | ±0.12 |
| Blacks | 1 at 0, falling to 0 by 0.35 | — | ±0.08 |
| Whites | 0 below 0.6, rising to 1 at white and staying there | — | ±0.08 |

Shadows is zero at black and Highlights zero at white, because moving the ends
is what Blacks and Whites are for. Highlights reaches a little past white so
that recovery can take hold of the headroom the shoulder is about to compress,
and Whites stays at full weight above white so that raising it carries the
headroom along rather than crushing it into white.

**No combination can invert the tone scale.** Each window is built from
smoothstep, whose slope is at most `1.5 / width`, so an amplitude `a` perturbs
the slope by at most `1.5a / width`. The amplitudes above are chosen so that
the worst opposing pair sums to under one — Shadows against Blacks reaches
0.94, Highlights against Whites 0.81 — leaving every stage monotone, and a
composition of monotone stages monotone. This is a property to test at the
corners of the parameter space, not a hope.

**The order is Contrast, then Shadows and Highlights, then Blacks and Whites**
— the global shape, then the regions, then the ends. Exposure is not in this
chain at all: it is a linear multiply before the crossing (ADR 010).

**The shoulder is a bend in luminance with a fade in colour.** Above a knee at
`1 − 0.5·(amount/100)`, the excess is compressed by `t/(1+t)`: slope one where
it begins, so a gradient shows no edge there, and approaching white without
reaching it, so two values that would both have clipped stay apart. As a value
is pulled down by a ratio `r`, its chroma is scaled by `r^1.5` toward the
neutral of the same luminance, because something genuinely overexposed loses
colour as it brightens.

**Its amount may be zero**, and zero means a hard clip. The stage is always in
the chain — that is what ADR 010's "mandatory" protects, and why the default is
a gentle roll rather than none — but the control has a true neutral, because a
photographer may want a clip and a number that secretly means "the minimum"
misleads.

## Consequences

- **Default development is no longer the identity above the knee.** A
  photograph developed with nothing set has its brightest eighth bent toward
  white. `develop`'s documentation and the command line's help say so; the test
  that asserted a stored ramp arrives unchanged now turns the roll-off off,
  which is what it always meant.
- **Contrast and the shoulder are a pair.** Contrast pushes values above white
  by design, so a build that had the controls without the shoulder would clip
  worse than no controls at all. The shoulder is therefore the part that was
  implemented first.
- **A saturated specular can still exceed one per channel.** The shoulder
  bounds luminance, not channels, and the fade toward white reduces but does
  not eliminate the overshoot; the output conversion clips what is left. Making
  that impossible would mean a per-channel transform, which this ADR rejects
  for the reason given above.
- **The five controls are decided here and not yet built.** Only the shoulder
  and its setting exist; the windows arrive with the perceptual crossing stage
  they act in, which is also when `NamedEncoding` gains the name ADR 011 asks
  for.
