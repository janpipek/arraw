# Colour Grading tints three tonal zones in Oklab

Colour Grading is the pointwise colour control that tones the photograph by
tonal zone: a hue and a saturation each for the Shadows, the Midtones and the
Highlights, with Balance and Blending to shape the zones. It is a leaf of the
settings table like the others (ADR 008), resolves into the plan's pointwise
group (ADR 011), and runs on the CPU and the GPU from the same arithmetic. The
maths is `main`'s (its ADR 0052), ported; the zone coordinate, the sign of
Balance and the placement are re-decided for the rewrite.

## Decision

**Storage.** `ColorGradingSettings` holds three `GradeZone`s (`shadows`,
`midtones`, `highlights`, each a `hue` and a `saturation`), `balance` and
`blending`. The keys are `gradeShadowHue`, `gradeShadowSaturation`,
`gradeMidtoneHue`, `gradeMidtoneSaturation`, `gradeHighlightHue`,
`gradeHighlightSaturation`, `gradeBalance` and `gradeBlending` (group
ColorGrading, Applicable to every photograph, affecting the pointwise stage).
Ranges: hue 0 to 360 degrees, saturation 0 to 100, Balance -100 to 100,
Blending 0 to 100. Defaults: 0, 0, 0 and 50. Only the `arraw:` namespace is
written; there is no `crs:` mapping.

**Hue 0 to 360, not 0 to 359.** `main`'s 359 belongs to an integer slider.
Hues here are floats, where 0 and 360 are the same hue; 0 to 360 also accepts
a value that wrapped to 360.

**The hue is an Oklab hue angle, not Lightroom's.** It is measured in Oklab's
a-b plane from +a toward +b, so roughly 30 is red, 110 yellow, 140 green and
260 blue. Lightroom's wheel puts red near 0, yellow near 60, green near 120 and
blue near 240, so a recipe written for Lightroom (say, a sepia at highlights
hue 40) gives another colour here. `main` used the same Oklab convention; a
mapping from Lightroom's wheel would only be worth it with `crs:` import, which
is not a goal. The settings header and the command-line and Python help say
so.

**Hues wrap in the plan; entry points still refuse them.** A hue is an angle,
so the plan wraps one outside 0 to 360 onto the wheel with `fmod` (400 is 40,
-30 is 330) instead of clamping it (which turned 400 into 360, that is red).
Only raw C++ settings reach the plan unvalidated. The sidecar, JSON, the
command line and Python keep refusing a hue outside 0 to 360, like every
other ranged setting, through the one descriptor table: a value out of range
there is far more often a typo than an intended angle, refusing it keeps the
generic validation free of a special case, and a sidecar is never silently
rewritten to another number than it holds.

**Off means off.** With all three saturations at zero nothing is tinted, so
the plan resolves to its default block whatever the hues, Balance and Blending
say. The plan, the GPU bytes and the pixels are then identical to the
defaults, and a change of hue alone does not invalidate a render. Non-finite
values are refused (`std::invalid_argument`) even when grading is off;
out-of-range values are clamped, as everywhere pixel maths reads a setting,
save the hues, which wrap (above).

**The maths.**

1. Each zone's tint is an Oklab offset resolved once, in the plan:
   `(a, b) = 0.10 * (saturation / 100) * (cos hue, sin hue)`. The hue is
   measured in Oklab's a-b plane from +a toward +b. Neither backend takes a
   sine or cosine per pixel, and both add identical vectors. 0.10 is `main`'s
   chroma at full saturation: a tint clearly visible on a grey, far below the
   chroma of a saturated colour.
2. The colour's tonal position is `toPerceptual(clamp(Y, 0, 1)) + shift`,
   clamped to 0 to 1, where `Y` is the working-space luminance and
   `toPerceptual` is the `y^(1/2.2)` coordinate the tone controls use
   (ADR 013), so Shadows and Highlights mean here the region they mean in the
   tone controls. `main` used the same coordinate by another name.
3. Each zone is a Gaussian bell `exp(-((position - centre) / width)^2)` with
   its centre at 0, 0.5 and 1. The three are normalised to sum to one, so
   the tint never exceeds the strongest zone's, and at pure black or white
   the end zone dominates (from 0.77 of the weight at Blending 100 to 1.0 at
   Blending 0). Blending sets the width linearly from 0.18 at 0 to
   0.45 at 100, `main`'s constants.
4. The weighted tint is added to the colour's Oklab a and b, using the
   rewrite's existing `toOklab` and `fromOklab` (ADR 027). Oklab L is
   untouched, so grading holds perceived lightness, and is a colour control,
   not a tone control. There is no per-zone luminance (Lightroom has one),
   for the reason every chroma control here has none: tone by zone is the job
   of Shadows, Highlights and the Tone Curve.
5. **The tint fades out toward white.** The blended tint is multiplied by
   `1 - smoothstep(0.85, 1.0, L)`, where `L` is the Oklab lightness of the
   colour being graded (`tintFadeStart` and `tintFadeEnd` in
   `ColorGrading.cpp`, mirrored by the shader). Below 0.85 the tint is
   whole; from there it falls smoothly to none at 1, and stays none above, so
   white stays white. The reason is the placement (below): the grade runs
   after the shoulder, and nothing after it rolls a channel back, so a full
   highlight tint on a near-white grey would push channels to 1.2 to 1.7, and
   the export would clip them into a flat band of shifted hue and lost
   lightness, in exactly the tones the Highlights zone is for. A fade by L is
   cheap, the same arithmetic on both backends, and needs only the L the code
   already computes. A colour whose fade is zero (L at least 1) is returned
   as it came, without the Oklab round trip. A gamut-exact chroma limit was
   the alternative, exact but costlier and with harder parity between the
   backends; the fade bounds the overshoot rather than removing it
   (Consequences).
6. **Balance follows Lightroom's sign.** `shift = balance / 100 * 0.25`.
   Positive Balance reads every colour as lighter, so more of the range falls
   to the Highlights zone; negative gives it to the Shadows zone. `main` ran
   the same shift; the sign is restated because an inverted one would mirror a
   grade imported from Lightroom.

There is no Global zone: `main`'s four-way plan was never needed, and the
Midtones zone with a wide Blending covers the use.

**Placement.** The block is part of `ColorAdjustmentPlan`, as `grading`, and
so of the pointwise group. In the chain it runs last in the colour block,
after Saturation, Vibrance and HSL, or after the Black & White mix. Unlike
every control before it, `adjustColor` no longer returns early for Black &
White: either branch's result goes through the grade, so a grey photograph is
toned too, which is the reason the feature exists. The grade then sees the grey
the mixer made, not the colour it came from. The shader's `adjustColor`
mirrors this one for one.

**GPU block.** The uniform block grows from 272 to 320 bytes, every offset
asserted: `uint grades` at 272 (the block is active), `float gradeBalanceShift`
at 276, `float gradeZoneWidth` at 280, a padding word, `vec4
gradeShadowMidtoneTint` at 288 (a, b, a, b) and `vec4 gradeHighlightTint` at
304 (the first two values used). No new probe stop is needed: the existing
ones still cut the chain where they did, and the final developed probe covers
the grade.

## Differences from `main`

- Zone weights from the perceptual coordinate `y^(1/2.2)` the rewrite already
  has, rather than a coordinate private to the grading code.
- The tint offset is resolved on the host into (a, b) pairs rather than a hue
  in degrees uploaded to the shader.
- Hue range 0 to 360 (floats), not 0 to 359, and an out-of-range hue that
  reaches the plan wraps rather than clamps.
- Placement relative to the highlight roll-off: `main` graded before Filmic
  Highlights, which then partly bleached a strong highlight tint; the rewrite
  grades after the shoulder, as the last stage of the colour block (ADR 027).
- The tint fades out between Oklab L 0.85 and 1, which `main` did not do. A
  grade on light tones is therefore weaker than `main`'s there, and a grade on
  white is none.
- No `crs:` attributes and no Global zone. Lightroom's per-zone Luminance was
  already out of `main`'s scope and stays so.
- Black & White is graded by the same code path, not a second one.

## Consequences

- The pointwise group of the plan, and so the checkpoint boundary (ADR 011),
  includes the grade: moving any grade setting recomputes the pointwise pass
  and everything after it, and nothing before.
- Parity between the backends holds to the documented pointwise tolerance
  (relative 3e-5) and, as for the colour block generally, to the
  ill-conditioned one (5e-4) where the source has negative channels or the
  grade follows controls that are themselves ill-conditioned. The CPU stays
  the reference. The tests assert Oklab lightness is held to 2e-5, the weights
  sum to one, grading off is bit-identical, the fade is continuous, leaves
  colours below L 0.85 bit for bit as without it and colours at or above
  white untouched, out-of-range hues wrap, and both backends agree over
  colour, Black & White, both extremes of Balance and Blending, hues without
  saturation, and highlight tints near white.
- Settings, sidecar, JSON, the command line (`--grade-shadow-hue` and its
  seven siblings) and Python (`GradeZone`, `ColorGradingSettings`,
  `DevelopSettings.color_grading`, flat keys `grade_shadow_hue` and so on) follow
  from the descriptor table. `arraw-cli info` lists a grade value that differs
  from its default.
- The develop panel does not show the grade yet; it comes with the GUI phase of
  the global adjustments plan. The panel's test lists the eight keys as not
  shown.
- Because the grade follows the shoulder, nothing downstream brings its
  overshoot back: values stay live in the buffer, but the export clips them
  (ADR 010, "Exports clip without the shoulder"). The fade toward white is
  what keeps a highlight tint in range, and it bounds the overshoot rather
  than removing it. On greys, measured over every hue: up to saturation 50
  every channel stays within 0 and 1, in the linear Rec.2020 working space and
  in sRGB alike; at saturation 100 a blue tint (Oklab hue about 266) still
  peaks about 14% over white near L 0.88 in the working space (about 20% once
  converted to sRGB for export), against 66% without the fade. The tests pin, on both backends, the review's cases (a
  full highlight tint at hues 30, 90 and 260 on greys at L 0.95 and 1, and
  on a linear grey of 0.9) within 0 and the larger of 1 and the input's
  largest channel; the CPU tests also pin every grey in range at saturation
  50. A saturated colour can still be pushed out of gamut anywhere below the fade; that is not
  clipped here, as for the other colour controls.
