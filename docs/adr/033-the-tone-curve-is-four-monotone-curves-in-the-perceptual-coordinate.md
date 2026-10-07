# The tone curve is four monotone curves in the perceptual coordinate

The Tone Curve is the pointwise control that `main` ran between tone and the
colour block. It is a leaf of the settings table like the others (ADR 008),
resolves into the plan's pointwise group (ADR 011), and runs on the CPU and the
GPU from the same arithmetic. Unlike every setting before it, its value is not
a number or a handful of named numbers but a list of points, so it brings a new
kind of leaf with it.

## Decision

**Storage.** `ToneCurveSettings` holds four `ToneCurve`s: `luma`, `red`,
`green` and `blue`, with keys `toneCurveLuma`, `toneCurveRed`, `toneCurveGreen` and
`toneCurveBlue` (group ToneCurve, Applicable to every photograph, affecting the
pointwise stage, no range). A curve is an ordered list of control points
`(x, y)`, both coordinates from 0 to 1 and held as floats like the other float
settings. The default is the identity, `{(0, 0), (1, 1)}`.

A well-formed curve (`isWellFormed`) has 2 to 16 points, finite coordinates
from 0 to 1, x increasing by at least `minimumCurvePointSpacing` (0.01), the
first point at x = 0 and the last at x = 1. The ends are fixed in x so that a
curve is a function on the whole of [0, 1] and the widget can drag them only
vertically. The spacing keeps points apart by about ten entries of the table
below, so that the curve passes through every point; closer points would fall
between two entries, and the curve would miss them.

Every frontend sorts: a list of points from outside is read through
`curveFromPoints`, or, in Python, through `normaliseCurvePoints`, which does the
same sorting and snapping. They sort the points by x and snap an end x within
`curveCoordinateTolerance` (1e-6) of 0 or 1 onto it, and it allows the spacing
the same slack, so a coordinate written as a decimal and read back as a float
still passes. Nothing else is repaired. An end x further from 0 or 1, a
repeated or too-close x, a wrong point count or a non-finite coordinate makes
the curve malformed. An x is never clamped, because moving a point along x
makes it a different curve. The codec skips a malformed value with a
`SettingMalformed` warning. A y outside 0 to 1 is clamped with a warning, as
every number is. The command line refuses any coordinate outside 0 to 1 as a
usage error (ADR 008), then goes through `curveFromPoints` too. Python sorts
and snaps in the `ToneCurve` constructor, in `replace` and in the flat `with_`,
but does not otherwise validate: a malformed curve is refused when it is used.
The sidecar and the command line share one parse of `"x,y;x,y"`
(`parsePointList`), each applying its own policy to the values. `validate` and `planFor` throw for a curve that is not
well formed, so a caller that builds settings by hand hears about it before a
render.

**One new leaf kind.** The accessor variant gains `ToneCurve&`, and the codec's
`Encoded` gains `PointList`, a vector of `(double, double)`. JSON spells it
`[[x, y], ...]` and the XMP attribute `"x,y;x,y;..."`. The floats are encoded
as the doubles of their shortest decimal spelling, so a point placed at 0.3
writes 0.3. The command line takes the XMP spelling, and Python takes a list of
`(x, y)` tuples (not bytes or text).

**Monotone cubic interpolation.** Between points the curve is a monotone cubic
Hermite spline in the Fritsch-Carlson construction. Tangents start as the mean
of the neighbouring secants, and the end ones as the end secant. A tangent is
zero at a turning point, where the secants on either side differ in sign, and
beside a flat segment. Then each segment's pair of tangents is scaled down,
where needed, to the radius-3 limit that keeps the segment monotone. A curve
drawn rising never dips, and dragging a point never makes a neighbouring
segment ring. `main` and Lightroom use a natural cubic spline. It is smoother
on wild curves, but it overshoots and then needs clamping, and a photographer
who draws a rising curve expects a rising curve.

**A table of 1024 entries.** The plan holds each curve resolved, not its
points: `CurvePlan` is a flag and a table of 1024 floats over [0, 1], linear
between entries, with the end entries set to the end points exactly.
Evaluation is two reads and a blend whatever the curve's shape. Plan equality
compares exactly what the pixels will see (ADR 011), and the GPU reads the same
table. The identity resolves to an inactive plan with a zero table, so default
settings leave every pixel bit-for-bit as it was and cost one branch per curve.
A curve that is the identity but for being spelled with a collinear middle
point is not detected. It gets an almost-identity table, which is harmless.

**Above 1 the curve rises at slope one.** Values above white stay live until
the output transform (ADR 011), so the curve cannot stop at x = 1. Above it,
the curve continues as `curve(1) + (x - 1)` in the perceptual coordinate. That
is slope one, offset by what the curve did to white. Headroom stays distinct
for the shoulder to roll off. `main` continued along the end secant instead.
That made brightness above white depend on where the last points sat: a steep
last segment multiplied the headroom five times or more, and a flat one
erased it. A flat end also turned an infinite input into NaN (0 times
infinity). With slope one, infinity stays infinity on both backends. The
extension never uses the end tangent, so a tangent the limiter cut short leaves
no step at 1.

**The perceptual coordinate.** The curve acts on `y^(1/2.2)` (ADR 010), the
same coordinate as the other tone controls and the one ADR 010 names for a
curve widget's x-axis. `main` used the sRGB transfer function here; one
perceptual coordinate in the engine is better than two.

**Below black.** Only a colour outside the working gamut has a negative
channel, and the power has no negative side, so a negative channel cannot enter
a red, green or blue curve. It passes through moved by the curve's lift
instead: `value + toLinear(curve(0))`, in linear light. That meets the curve
continuously at zero, keeps the colour as far outside the gamut as it was, and
is the identity below zero when the curve keeps black at 0. An earlier draft
sent negative channels to `curve(0)`. Nudging one highlight point then clipped
every out-of-gamut colour in the picture and shifted its hue, and clipping
belongs to the output transform alone. A NaN channel takes the curve's value at
black, as zero does. A channel whose curve is inactive is left bit-identical.

**Placement.** After `shapeTone`, before `rollHighlights`, so the curve sees
the result of Basic Tone, as in Lightroom, and ADR 011's curve input tap sits
between them. The shoulder still catches whatever the curve lifts past white.
After the shoulder the curve's x-axis would be the displayed values, but a
curve there could clip, and clipping belongs to the output transform alone.

**Luma by the ratio, with the lift as a neutral.** The luma curve is evaluated
at the luminance in the perceptual coordinate, and the colour follows by the
ratio, as in `shapeTone`, so hue and saturation come through. A curve that
lifts black is split in two. Its lift, `toLinear(curve(0))`, is added to every
channel as a neutral. Only the rest, `toLinear(curve(x)) - lift`, scales the
colour by the ratio to the luminance. A grey lands exactly on the curve.

A lifted curve still climbs out of zero infinitely steeply in linear light, so
the ratio of that rest grows as the luminance falls. For a colour outside the
gamut with almost no luminance but large channels, it would grow without
bound: a red of 1 against the green that cancels its luminance came out in the
thousands, and it jumped to neutral where the luminance crossed zero. Below
`curveRatioFloor` (2^-14, fourteen stops under white) the ratio is therefore
held at its value at the floor. There the curve is the straight line, in linear
light, from its lift to its value at the floor, continued through zero into
negative luminance. The ratio is bounded, the result is continuous across zero
and across the floor, and on a curve lifting black to 0.2 the line differs from
the curve by under 3% of the value. A colour with negative luminance therefore
keeps its colour, scaled and lifted, rather than becoming a neutral. A NaN
luminance has no ratio at all and becomes the neutral lift. `shapeTone` keeps
its own lifted-black branch, which is unchanged.

**Red, green and blue curves act per channel.** They run after the luma curve,
each on its own channel in the perceptual coordinate, and may shift hue, which
is their purpose.

**A texture on the GPU.** The four tables travel as one 1024x1 RGBA32F texture
(R luma, G red, B green, A blue), bound as a second input of the Pointwise
pass beside the source. The uniform block grows from 256 to 272 bytes with four
flags. The shader's lookup uses `texelFetch` and hand interpolation with the
CPU's index, weight and extension arithmetic, rather
than a sampler's filtering, so both backends take the same steps. A 16 KB
uniform array would also have fit the guaranteed minimum, but a texture leaves
the block small. The texture is uploaded whenever the Pointwise pass runs
with a curve active; with none active, nothing is uploaded and the source is
bound in its place, since the shader never reads it. A resume from a Pointwise or later
checkpoint starts after that pass and uploads nothing. The pointwise probe gains
an `AfterCurves` stop.

## Consequences

- The pointwise group of the plan, and so the checkpoint boundary (ADR 011),
  includes the curves: moving a curve recomputes the pointwise pass and
  everything after it, and nothing before.
- Resolving four tables costs four thousand evaluations when the plan is built,
  and the plan is four tables larger. The plan is built once per settings
  change, not per pixel.
- Parity between the backends holds to the documented tolerance for ordinary
  curves on in-gamut sources. Three cases use the ill-conditioned tolerance.
  An inverting curve sends bright inputs to near black, where the power back
  to linear amplifies the rounding of the table blend. Negative channels and
  luminances near zero are the other two, as they are for the tone controls.
  Negative channels now pass through the curves rather than being clipped, so
  they reach the colour block, which is ill-conditioned on them.
- Settings, sidecar, JSON, the command line (`--tone-curve-luma`,
  `--tone-curve-red`, `-green` and `-blue`, each a malformed value being a
  usage error) and Python (`ToneCurve`, `ToneCurveSettings`) follow from the
  descriptor table. `arraw-cli info` shows a curve as its points and leaves it
  out when it is the identity.
- The develop panel does not show the curves yet; it needs a curve editor and
  the curve input histogram, which are the later phases of the global
  adjustments plan. The panel's test lists the four keys as not shown.
- Curves in XMP use this engine's own attribute and 0 to 1 coordinates, not
  Lightroom's `ToneCurvePV2012` in 0 to 255. Reading Lightroom's is a matter of
  conversion, and out of scope for now.
