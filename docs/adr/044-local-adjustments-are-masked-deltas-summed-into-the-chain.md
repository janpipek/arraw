# Local adjustments are masked deltas summed into the chain

The [feature brief](../desired-features.md) asks for up to 16 masked
adjustments per photograph: **linear** (a graduated fade), **radial** (an oval
with feather and invert) and **brush** (a stencil painted on the photograph).
Each mask carries its own tone and colour deltas. The
[local adjustment plan](../ideas/local-adjustment-plan.md) chose how; its
Decisions table (user, 2026-10-08) is binding here. This ADR fixes what that
plan left to it: the model, the equations, the Temp/Tint law and its
constants, the shape maths, how Presence reads its context under masks, the
brush stroke contract, the GPU layout, the plan's grouping and taps,
persistence and the front ends.

It is written ahead of the code (step 0 of the plan). Statements about today's
code cite the file; everything else is what steps 1 to 6 build. The brush
parts marked *provisional* hold until the brush prototype (plan step 4), which
amends this ADR.

## Decision

### 1. The model

**`DevelopState` gains the list beside the settings** (`include/LocalAdjustments.h`,
`include/DevelopState.h`). `DevelopState.h` already reserves the place: edits
that belong to one image are siblings of `settings`, as lists with stable ids
whose large payloads are shared.

```text
DevelopState
  settings                 DevelopSettings                 (unchanged)
  localAdjustments         std::vector<LocalAdjustment>    at most 16, in order
  nextLocalAdjustmentId    LocalAdjustmentId               the counter, persisted

LocalAdjustment
  id        LocalAdjustmentId   unique in the state
  name      std::string         empty: the front end shows "Linear 2" and the like
  enabled   bool
  opacity   float               0 to 1, scales every delta
  invert    bool
  shape     Mask                std::variant<LinearMask, RadialMask, BrushMask>
  deltas    LocalDeltas         one float per row of the local table (section 2)

LinearMask  from, to: CorrectedPoint          weight 1 at from, 0 at to
RadialMask  centre: CorrectedPoint; radiusX, radiusY (long-edge units);
            angle (degrees); feather (0 to 1)
BrushMask   strokes: std::shared_ptr<const StrokeList>   (section 6)
```

`CorrectedPoint` and `SensorPoint` are new public types: positions normalised
per axis to the frame (ADR 009), distinct from each other and from
`SourcePoint` and `UprightPoint` (`src/core/GeometryPlan.h`, which are in
source edge units). Without lens corrections both equal the source normalised
by its own size, and convert by named functions only.

**Ids.** `LocalAdjustmentId` wraps a `std::uint32_t`. The counter starts at 1.
Adding or duplicating a mask takes the counter's value and increments it, so a
duplicate gets a new id. Removing a mask never lowers the counter. Undo restores
the counter with the state, so an id freed by undo may be handed out again; no
reachable state then holds both. Reading a document sets the counter to the
larger of the stored counter and the largest id read plus one.

**Edit rules live in core**, beside `Edits.h` (`include/LocalAdjustmentEdits.h`):
add (linear, radial, brush), duplicate, remove, reorder, rename, enable,
invert, set opacity, set a delta, move a handle, append a stroke. Each takes a
state and returns a new one, as `withValue` does.
- Adding or duplicating when the list holds 16 throws `std::invalid_argument`
  and leaves the state unchanged. `canAddLocalAdjustment(state)` tells a front
  end whether to grey the action out.
- An id that is not in the list throws `std::invalid_argument`.

**Validation** (`validate(const DevelopState&)` and every edit rule):
- Every number must be finite; a non-finite one is refused.
- Out-of-range values are clamped, as for settings (ADR 008): opacity and
  feather to 0 to 1, deltas to their local range (section 2), positions to
  -2 to 3 per axis, radii and brush radii to at most 4 long-edge units, angle
  wrapped into [-180, 180).
- Degenerate geometry is refused: a linear mask whose endpoints are closer than
  `minimumMaskExtent` = 0.001, or a radial radius below it. A state knows no
  frame size, so the endpoints' distance is measured in normalised
  coordinates, `sqrt(du^2 + dv^2)`; in the isotropic metric of section 4 that
  is at least `0.001 * min(W, H) / L`, never zero. Radii are already in
  long-edge units.
- Duplicate ids, an id of zero, an id at or above `nextLocalAdjustmentId`, or
  more than 16 entries, are refused. (Reading repairs the counter first, as
  above, so only a state built in code can fail the counter check.)
- Handles may lie outside the image.

**Equality is semantic.** `BrushMask` compares equal when its pointers are
equal or, failing that, when the stroke lists' contents are. A state copied
and unchanged is never a spurious edit, and a reloaded one equals the saved one.

**View state stays out.** The selected mask, overlay visibility, the active
tool and a stroke being painted belong to the window.

**Tests.** A seventeenth add or duplicate throws and leaves the state equal;
an unknown id throws; a duplicate gets a new id; removing never lowers the
counter; each validation refusal and clamp above; a brush mask equal by
contents behind a different pointer.

**`shape` can later become a list of components with an operation** (add,
subtract, intersect) without changing anything around it: everything below
reads a mask's weight, never its shape.

### 2. The equations

**The local table** (`localAdjustmentDescriptors`, next to
`developSettingDescriptors` in `include/SettingDescriptors.h`): key, range,
default 0, the global control it acts on, and its position in the chain. It
drives JSON and XMP keys, Python names, slider rows and history wording. Its
order is the panel's (Lightroom's Basic order) and the GPU's array order:

| # | Key | Python | Local range | Global control (its range) | Chain position |
|---|---|---|---|---|---|
| 0 | `relativeTemperature` | `relative_temperature` | -100 to 100 | none | after `toWorking` |
| 1 | `relativeTint` | `relative_tint` | -100 to 100 | none | after `toWorking` |
| 2 | `exposure` | `exposure` | -4 to 4 EV | Exposure (-5 to 5) | exposure gain |
| 3 | `contrast` | `contrast` | -100 to 100 | Contrast (-100 to 100) | `shapeTone` |
| 4 | `highlights` | `highlights` | -100 to 100 | Highlights | `shapeTone` |
| 5 | `shadows` | `shadows` | -100 to 100 | Shadows | `shapeTone` |
| 6 | `whites` | `whites` | -100 to 100 | Whites | `shapeTone` |
| 7 | `blacks` | `blacks` | -100 to 100 | Blacks | `shapeTone` |
| 8 | `texture` | `texture` | -100 to 100 | Texture | `applyPresence` |
| 9 | `clarity` | `clarity` | -100 to 100 | Clarity | `applyPresence` |
| 10 | `dehaze` | `dehaze` | -100 to 100 | Dehaze | `applyPresence` |
| 11 | `saturation` | `saturation` | -100 to 100 | Saturation | `adjustColor` |
| 12 | `vibrance` | `vibrance` | -100 to 100 | Vibrance | `adjustColor` |

The global ranges are those of `include/ToneSettings.h`,
`include/ColorSettings.h` and `include/PresenceSettings.h`.

**The weight.** Mask `i` has a weight `w_i` in [0, 1] at every pixel
(sections 4 and 6), inverted to `1 - w_i` when `invert` is set. The plan
holds `k_i,c = opacity_i * delta_i,c`, worked out once on the host in float.

**The sum, then one clamp, then the global resolution.** For every control
`c` of the table and every pixel:

```text
s_c = 0;  for i in list order: s_c += w_i * k_i,c          (float)
if s_c == 0:  the pixel uses the plan's global value for c, as resolved today
else:         e_c = clamp(g_c + s_c, low_c, high_c)
              then e_c is resolved by the same expression the global setting uses
```

`g_c` is the global setting in setting units, already clamped (zero for
Temp and Tint), and the clamp is the global control's range (the local range
for Temp and Tint, which have no global counterpart). Summing first and adding
the global value last makes two masks that cancel exactly cancel to the
global value. Clamping once, to the global range, keeps ADR 013's
monotonicity argument: every pixel's tone scale is one the global sliders
could produce.

**Resolution per control** (each is the expression in today's code, read per
pixel instead of once):

| Control | Per pixel, when `s_c != 0` | Today's expression |
|---|---|---|
| Exposure | `gain = exp2(e)` | `tonePlanFor`, `src/core/ToneSettings.cpp` |
| Contrast | `slope = exp2(e / 200)`, `scale = pow(greyPivot, 1 - slope)` | `contrastSlopeFor` |
| Highlights, Shadows | `0.12 * e / 100` | `toneShiftFor` with `regionalReach` |
| Whites, Blacks | `0.08 * e / 100` | `toneShiftFor` with `endpointReach` |
| Texture, Clarity, Dehaze | `e / 100` | `presencePlanFor`, `src/core/Presence.cpp` |
| Saturation, Vibrance | `e / 100`, applied when `e != 0` | `colorAdjustmentPlanFor`, `src/core/ColorAdjustments.cpp` |
| Temp, Tint | the log-gain law (section 3) | none |

**What becomes per-pixel.** These plan fields gain a per-pixel value where a
pixel's local sum is non-zero: `exposureGain`, `contrastSlope`,
`contrastScale`, `shadowShift`, `highlightShift`, `blackShift`, `whiteShift`
(`src/core/ProcessingPlan.h`), `PresencePlan::texture`, `clarity` and `dehaze`
(`src/core/Presence.h`), `ColorAdjustmentPlan::saturation`, `vibrance` and
their `adjusts…` flags (`src/core/ColorAdjustments.h`), and the new
Temp/Tint gain. `shapesTone` becomes per-pixel too: a pixel shapes tone when
the plan does or any of its five tone sums is non-zero.

**What stays global.** The shoulder (`shoulderKnee`), the tone curves, HSL,
Black & White, Colour Grading, `toWorking`, noise reduction, geometry and
effects. A Presence term whose effective amount is zero contributes nothing,
as a global one at zero does today.

**Flags.** The plan's local block holds `touched`, one bit per control that
any mask in the block carries, so the per-pixel loop sums only those; a
control no mask touches costs nothing. Contrast is the expensive one (an
`exp2` and a `pow` a pixel): only `touched.contrast` makes the chain resolve
it per pixel.

**Neutral cases are bit-identical.**
- No masks, every mask disabled, every mask at opacity 0, or every delta zero:
  the local block is empty (disabled masks, and masks whose every `k` is zero,
  are left out of the plan), the chain takes exactly today's path, and the output is today's bit
  for bit on each backend.
- With masks present, a pixel where every local sum is exactly zero takes the
  plan's global values, so it is bit-identical to the render without masks on
  the same backend. This holds outside every mask's coverage and where masks
  cancel exactly (equal weights and opposite `k`).

**Tests.** Each of the eleven non-Temp/Tint controls: a full-weight mask of
`k` on a global `g` equals the global render at `g + k` (to float tolerance),
including past the clamp; the neutral cases above, bit for bit; two masks
cancelling; sixteen overlapping masks against the same sum written as one.

### 3. Relative Temp and Tint

**A diagonal gain in the working space, right after `toWorking`**, where white
balance lives, and before exposure. With `T` and `N` the clamped sums of
`relativeTemperature` and `relativeTint`:

```text
log2 g_R = 0.16 * T/100 + 0.19 * N/100
log2 g_G = 0
log2 g_B = -0.43 * T/100 + 0.31 * N/100
g <- g / (0.2627 g_R + 0.6780 g_G + 0.0593 g_B)
colour <- g * colour            (per channel)
```

Positive Temp warms, as raising the global temperature does; positive Tint
moves toward magenta, as the global Tint does (`include/WhiteBalance.h`: a
positive tint "shifts the photograph toward magenta"). Because the gains are
exponentials of linear functions of `T` and `N`, summing in setting units
is summing log gains (up to the normalisation, one scalar applied after the
sum): two masks of Temp 50 overlapping fully equal one of Temp 100. When `T` and `N` are both exactly zero no gain is applied, which
keeps the neutral case bit-identical whether or not `Y(1, 1, 1)` rounds to exactly 1 in float.

**Normalised by the working-space luminance row** (`colorspaces::workingLuminance`,
Rec.2020's 0.2627, 0.6780, 0.0593, `src/core/ColorSpaces.h`). The gain acts on
working-space colours, and every later stage that reads brightness
(`shapeTone`, `applyToneCurves`, `rollHighlights`, `applyPresence`) measures it
with that row. So a neutral keeps exactly the luminance every later stage sees,
and a local Temp never brightens or darkens greys. The as-shot row
(`asShotLuminanceRow`, ADR 039) maps *source* channels before `toWorking`, so it
belongs to a different space; it would make the normalisation camera-dependent
and different for a RAW and a JPEG. The Presence detail is still measured on
the source colour before both white balances (`developToCurveInput`), so a
local Temp, like the global one, never changes what Texture, Clarity or Dehaze
see.

**How the constants were obtained.** A throwaway program (not in the
repository) linked `libarraw` and called `whiteBalanceGains`
(`src/core/WhiteBalance.cpp`) at 5500 K / tint 0 and at 7000 K, 4000 K,
tint +40 and tint -40. For each it took the change of gains `d = G(L1) / G(L0)`
in camera space, mapped a neutral through it into the working space
(`toWorking * diag(d) * toWorking^-1 * (1, 1, 1)`, which is how a grey
lit by 5500 K renders under the new setting), normalised by the luminance
row, and read the log2 ratios of red and blue to green. A ±1500 K step around
5500 K is -68 / +39 mired, so the two directions differ; the constants are their
mean (+T from 7000 K, the negated 4000 K step; +N from tint +40, the negated
-40 step):

| Camera | Temp: R/G, B/G (log2) | Tint: R/G, B/G (log2) |
|---|---|---|
| Working space (a camera whose primaries are Rec.2020, unit calibration) | +0.164, -0.431 | +0.189, +0.306 |
| Sony ILCE-7M3 (LibRaw's Adobe matrix, rows normalised as LibRaw does) | +0.168, -0.446 | +0.192, +0.309 |
| Canon EOS 5D Mark IV (the same) | +0.144, -0.436 | +0.198, +0.306 |

The working-space row was recomputed independently against the same library
and agrees to the third decimal. The two camera matrices were transcribed
from memory, not checked against LibRaw's table (there is none in the
container); they are a plausibility check, and the constants do not depend
on them.

The law applies in the working space for every photograph, so the constants
come from the working-space camera, rounded to two decimals; real cameras land
within 0.03 stops of them. For a real camera the global change is not exactly
diagonal in the working space (off-diagonal mass 3 to 8% of the diagonal), so
"matches" means the neutral axis. The resulting gains, after normalisation:

| `T`, `N` | g_R | g_G | g_B | The global setting it stands for (measured) |
|---|---|---|---|---|
| +100, 0 | 1.100 | 0.985 | 0.731 | 7000 K: 1.070, 0.991, 0.799 |
| -100, 0 | 0.901 | 1.007 | 1.357 | 4000 K: 0.868, 1.009, 1.478 |
| 0, +100 | 1.085 | 0.951 | 1.179 | tint +40: 1.083, 0.952, 1.184 |
| 0, -100 | 0.917 | 1.046 | 0.844 | tint -40: 0.916, 1.046, 0.851 |

**Same law for RAW and non-RAW.** The working space is shared, so a JPEG gets
a local Temp although its global temperature is refused today
(`colorMatrixFor`, `src/core/ColorSettings.cpp`).

**Under Black & White it acts as a colour filter before the mixer.** The gain
changes a coloured pixel's hue, saturation and luminance before
`applyBlackAndWhite` makes it grey, so a warm local Temp lightens reds and
darkens blues in the grey. Neutrals keep their grey with a zero mix. This is
documented, not suppressed. Local Saturation and Vibrance, like the global
ones, do nothing under Black & White (`adjustColor`).

**Test.** ±100 on a neutral equals the table above to 1e-3; `T` and `N` of
zero leave every pixel bit-identical; Temp 50 + Temp 50 on full weight equals
Temp 100; a neutral's working luminance is unchanged to 1e-6.

### 4. Linear and radial shapes

**The isotropic metric.** A normalised point `(u, v)` of the corrected frame
of size `W x H` is measured as `P = (u * W / L, v * H / L)` with `L = max(W, H)`:
long-edge units (ADR 009), so a circle is a circle on screen and the bands of
a linear mask are perpendicular to its line. `W`, `H` are the size of the
source being rendered (a pyramid level, a half-size decode, or the full
source), as the geometry's normalised crop is (ADR 023).

**Linear.** With `A`, `B` the endpoints `from` and `to` in this metric and
`D = B - A`:

```text
t = dot(P - A, D) / dot(D, D)
w = 1 - smoothstep(0, 1, t)          (smoothstep as in ProcessingPlan.h)
```

One at `from`, zero at `to`, a smooth fade between, constant along lines
perpendicular to `from -> to`.

**Radial.** With centre `C`, radii `rx`, `ry` and angle `theta` (degrees,
turning the x radius from the frame's +x towards +y, which is clockwise on
screen before orientation; a mirrored orientation shows it the other way):

```text
q = R(-theta) (P - C);   d = sqrt((q.x / rx)^2 + (q.y / ry)^2)
inner = 1 - max(feather, p / min(rx, ry))
w = 1 - smoothstep(inner, 1, d)
```

`p` is one pixel of the rendered source in long-edge units (`1 / L`). One
inside `inner`, zero at and beyond the boundary. Feather 0 gives a hard edge
antialiased over one pixel along the short axis; any feather narrower than a
pixel renders the same at that level.

**Invert.** `w <- 1 - w`, for every kind. An inverted brush with no strokes
covers the whole frame.

**Resolved into source-pixel coefficients.** The plan never holds normalised
points. For the rendered source's size it holds, per mask, the coefficients
that take a pixel centre `(x + 0.5, y + 0.5)` in source pixels straight to
the shape's variable, composed in double and stored as float:
- linear: `t = alpha * (x + 0.5) + beta * (y + 0.5) + gamma`;
- radial: the centre `(cx, cy)` in source pixels, a 2x2 matrix `M` with
  `q' = M (x + 0.5 - cx, y + 0.5 - cy)` and `d = |q'|`, and `inner`.

A reduced preview and a full export therefore evaluate the same field at
their own pixels, and the GPU evaluates exactly what the CPU does. Pixel scale
does not enter the shapes: positions and radii are fractions of the frame.

**Region renders evaluate the same field.** The pointwise pass covers the
whole source and the region is cut after geometry (ADR 025), so a region with
masks is the crop of the whole render bit for bit. When the earlier stages are
later restricted to a region's footprint, linear and radial masks need no
margin, provided the coefficients keep taking whole-source pixel coordinates;
brush coverage must be cut at the same footprint.

**Tests.** Anchoring under crop, straighten, every orientation and flip;
preview against export; odd and non-square sizes; a region against the whole
render; feather 0 against narrow feathers; invert; masks outside the frame.

### 5. Texture, Clarity and Dehaze under masks

**The context's bases follow the reachable amounts, not the amounts.** For each
of Texture, Clarity and Dehaze the plan takes the interval every pixel's
effective amount lies in:

```text
low_c  = clamp(g_c + sum over the plan's masks of min(0, k_i,c))
high_c = clamp(g_c + sum over the plan's masks of max(0, k_i,c))
```

Weights lie in [0, 1], inverted or not, and opacity is already folded into
`k`, so `s_c` lies in the two sums' interval at every pixel; the clamp is
monotone, so every pixel's effective amount lies in `[low_c, high_c]`. It is
conservative: an empty brush, or a mask wholly outside the frame, still
counts. Disabled masks and masks whose `k` is zero are not in the plan
(section 2), so they never prepare a base.
- The fine base (Texture) exists when Texture's interval is not `[0, 0]`.
- The coarse base (Clarity) exists when Clarity's interval is not `[0, 0]`.
- Dehaze's floor exists when `high > 0`: the opening, reconstruction and
  floor blur of ADR 041.
- Dehaze's mean exists when `low < 0`: the broad blur of ADR 041.
- The coarse cells unblurred exist when Clarity's base or the floor does.

Both haze bases share one reduction of the coarse cells. `PresencePlan`'s one
`haze` base (`src/core/Presence.h`) becomes two, `hazeFloor` and `hazeMean`,
and `presenceContextFieldsOf` lists both. A plan with only a global Dehaze has
exactly the one its sign needs, with today's parameters, so its pixels do not
change. Cost on the GPU: thirteen renders for the floor and three for the mean
today; both share the reduction, so fifteen.

**Each pixel selects by the sign of its effective Dehaze.** Positive reads the
floor against the coarse cells; negative reads the mean; exactly zero reads
neither and changes nothing (no veil, no chroma). `PixelContext` gains
`hazeMean` beside the floor. Texture and Clarity read their bases only where
their effective amount is not zero.

**A base that does not exist is never read.** In exact arithmetic the interval
rule makes this automatic. In float the per-pixel sum and the host's bound
may round differently (and a shader compiler may contract or reorder), so at
an interval end that is exactly zero a pixel could come out a rounding error
past zero. The rule is therefore stated per pixel as well: a control whose
base for that sign is absent contributes nothing at that pixel. Every
Presence term is proportional to its amount, so the two readings differ by a
rounding error. The block carries which bases exist (the local header's
flags, section 7), and the CPU tests the plan's bases, not the amount's sign
alone.

**The context is still rebuilt with every pointwise run**, from the pixels at
the Denoise boundary (ADR 041; `runPointwise` in `src/core/Develop.cpp`,
`runPasses` in `src/gpu/GpuDevelop.cpp`). It depends on the bases that exist,
never on mask geometry, and on amounts only through which bases exist. Caching it apart from the pointwise run,
keyed by the Denoise prefix and `presenceContextFieldsOf`, remains an optional
performance step.

**Tests.** Mixed signs across one frame against two single-sign renders
composed by hand; masks that cancel to zero read neither base; local-only
Presence with global zero; global positive Dehaze with a negative mask; an
interval end exactly at zero (global Dehaze -50, one mask +50 at full weight:
no floor is prepared and none is read); the bases a plan prepares for given
globals and `k`, as a table test of the interval rule with opacity and invert;
both backends.

### 6. Brush: the stroke contract (provisional until the brush prototype)

```text
StrokeList   rasteriser: std::uint32_t; strokes: std::vector<Stroke>
Stroke       radius (long-edge units of the sensor frame), hardness (0 to 1),
             flow (0 to 1), erase (bool), points: std::vector<SensorPoint>
```

Strokes, not rasters, are stored, in the sensor frame (ADR 009). Pressure is
reserved and ignored.

**Dabs by distance.** Along each stroke's polyline, measured in the isotropic
metric of the sensor frame, a dab is placed at every arc length `k * s` from
the first point, `s = 0.25 * radius`, `k = 0, 1, …`, and one more at the last
point when the length is not a multiple of `s`. A single point is one dab. The
dabs depend on the path only, never on the pointer's event rate or the
resolution painted at: an extra point inserted on a straight segment changes
nothing. Extending a stroke keeps every dab at `k * s` and moves only the end
dab, so it changes coverage only near the stroke's end.

**Dab profile.** At distance `r` from the dab's centre, with
`inner = min(hardness * radius, radius - p)` (`p` one raster pixel in
long-edge units, so hardness 1 is a hard edge antialiased over a pixel):

```text
d = 1 - smoothstep(inner, radius, r)
paint:  m <- m + flow * d * (1 - m)
erase:  m <- m * (1 - flow * d)
```

Coverage `m` starts at 0. Strokes apply in list order, dabs in path order.
Each raster pixel is evaluated at its centre, `((i + 0.5) / Wr, (j + 0.5) / Hr)`
normalised, with `Wr x Hr` the raster's size. Radius, hardness, flow and erase
are captured per stroke when it begins.

**Rasteriser version.** `StrokeList::rasteriser` is 1 for the rules above. An
arraw keeps rendering every version it has written. A version it does not
know drops the mask with a warning (section 9).

**Rasterising and caching.** `rasteriseBrush(strokes, size, pixelScale)` is a
pure function, and the reference. A cache in front of it is an optimisation
that correctness never relies on:
- keyed by the stroke list (pointer, then contents), the raster size, the
  pixel scale and the rasteriser version;
- tiled, so an appended stroke re-rasterises only the tiles its dabs touch.

**During a gesture** the live stroke is a mutable buffer the window owns.
Commit freezes it into a new `StrokeList` that shares the previous strokes (a
persistent list, not a copy). Undo restores the previous list, whose tiles the
cache still holds; cancel drops the live stroke.

**Resolution.** Coverage is rasterised at the rendered source's size, one
coverage pixel per source pixel. One level coarser is the option the
prototype measures, with hardness 1 as the case to judge.

**Tests.** The same path sampled at different event rates (extra collinear
points) gives identical coverage; the same strokes at two raster sizes agree
after resampling within a stated tolerance; a reloaded stroke list rasterises
bit-identically; a cached and an uncached raster are equal; an extended stroke
differs from the original only within `radius + s` of the old end point and
within `radius` of the added path.

### 7. GPU

**The local block extends the pointwise uniform block.** `GpuContext` binds one
uniform buffer per pass, at binding 1 (`bindingsFor`,
`src/gpu/GpuContext.cpp`), so a second block would change every pass's layout.
`GpuPointwiseBlock` (`src/gpu/GpuPlan.h`, 384 bytes today) grows at its end,
std140, with `offsetof` `static_assert`s and the shader-layout reflection test
(`tests/gpu/test_GpuShaderLayout.cpp`):

```text
offset 384   uvec4 localHeader     count, touched bits, flags (which Presence
                                   bases exist, section 5), padding
offset 400   vec4  localGlobal[4]  g_c in setting units, table order (13 used)
offset 464   GpuLocalMask local[16], 112 bytes each:
               uvec4 kind, invert, coverage texture, coverage channel
               vec4  shapeA        linear: alpha, beta, gamma, 0
                                   radial: cx, cy, inner, 0
               vec4  shapeB        radial: M row-major; linear: 0
               vec4  k[4]          opacity * delta, table order (13 used)
size 2256
```

It is a fixed size and always uploaded (2 KB a render). Under the 16 KB
uniform block every target backend guarantees.

**Bindings.** 0 source, 1 the block, 2 tone curves, 3 fine base, 4 coarse
base, 5 coarse cells (as today), 6 haze floor, 7 haze mean (new; the image
stands in when off, as for every grid), 8 to 11 brush coverage. Twelve
bindings, eleven of them textures: within the 16 sampled textures per stage
that Vulkan and OpenGL ES 3.0 guarantee; D3D11 and Metal allow more.

**Brush coverage packing (provisional).** Four brush masks per RGBA8 texture,
up to four textures. Every QRhi backend supports RGBA8, so this needs no
format check. The CPU rasterises the coverage and quantises it to 8 bits, and
both backends read the same texels (`texelFetch` at the pixel, as the source),
so coverage adds nothing to the parity gap. A brush mask's
`coverage texture` and `channel` say where it lives. The prototype may choose
R16 or RGBA16F (banding) or a texture array instead. Packing reduces bindings,
not memory: 16 full-size brush masks are 384 MB at 24 MP in 8 bits.

**Parity.** One shader function per shape and per resolution of section 2,
mirroring the C++ of the same name line for line. Held to the pointwise
tolerance, 3e-5 relative (ADR 041). Cases: each control alone and all
together, narrow feathers, overlaps, cancellation, 16 masks, inverted masks,
odd sizes and pixel scales 1 to 8, mixed-sign Dehaze, a camera-native fixture.

### 8. Plan, checkpoints and taps

**The local block joins the Pointwise group.** `ProcessingPlan` gains
`LocalPlan local`: the resolved masks (kind, invert, coefficients, coverage
slot, `k`), the globals in setting units, `touched`, and each brush mask's
`std::shared_ptr<const StrokeList>` and raster size, compared as in section 1.
`stagesOf` (`src/core/ProcessingPlan.h`) lists it in the pointwise tie. A mask
edit therefore invalidates Pointwise onwards and never Denoise: it resumes
from the Denoise checkpoint, as a tone edit does (ADR 024). Presence base
changes caused by local amounts sit in `presence`, which is already in that
group.

`planFor(encoding, state, pixelScale)`, which knows no size, leaves the local
block empty, as it leaves Presence off. The buffer and `Photo` overloads
resolve it.

**The curve-input tap reads the local controls before it.** The chain order
(`developToCurveInput` and `developPixel` in `ProcessingPlan.h`, `main` in
`src/gpu/shaders/develop.frag`):

```text
toWorking -> local Temp/Tint -> exposure -> shapeTone -> applyPresence
  -> [curve input tap] -> tone curves -> shoulder -> adjustColor
```

So eleven local controls act before the tap: `relativeTemperature`,
`relativeTint`, `exposure`, `contrast`, `highlights`, `shadows`, `whites`,
`blacks`, `texture`, `clarity`, `dehaze`. Two act after it: `saturation`,
`vibrance`. (Dehaze's own chroma is part of Dehaze, before the tap.)

`curveInputFieldsOf` gains `preTapLocalFieldsOf(plan.local)`: the masks that
carry a non-zero `k` for one of the eleven, each with its kind, invert,
coefficients, coverage identity and those eleven `k`; and the eleven globals.
A Saturation or Vibrance mask edit, or moving a mask that only carries them,
leaves `sameAtTap` true and the curve histogram alone. Local Temp/Tint sits
before the GPU's `AfterMatrix` probe, so that probe still means "after white
balance"; its description in `src/gpu/GpuPlan.h` ("after the source-to-working
transform") changes to say so, and its CPU reference applies the local gain
too.

**Tests.** A Saturation-only or Vibrance-only mask edit, and moving a mask
that carries only those, leave `sameAtTap` true; any edit to one of the eleven
pre-tap controls, to a pre-tap mask's geometry or invert, or a reorder of
pre-tap masks makes it false; a mask edit resumes from the Denoise checkpoint
and never reruns noise reduction; a plan built twice from equal states
compares equal, brush masks included.

### 9. Persistence

**State JSON.** `stateToJson(const DevelopState&)` and
`applyStateJson(std::string_view, DevelopState base, DiagnosticLog&)` sit
beside `settingsToJson` and `applySettingsJson` (`include/SettingsJson.h`),
which stay settings-only. The document is the settings document plus one key:

```json
{"arraw": 1, "settings": {…},
 "localAdjustments": {"version": 1, "nextId": 4, "masks": [
   {"id": 3, "type": "radial", "name": "", "enabled": true, "opacity": 1,
    "invert": false,
    "geometry": {"centre": [0.5, 0.4], "radius": [0.2, 0.1], "angle": 30,
                 "feather": 0.5},
    "deltas": {"exposure": 0.5, "dehaze": 20}}]}}
```

A linear mask's geometry is `{"from": [x, y], "to": [x, y]}`; a brush's is
`{"rasteriser": 1, "strokes": [{"radius", "hardness", "flow", "erase",
"points": [[x, y], …]}]}` (provisional). Only non-zero deltas are written; an
absent one is 0. When `localAdjustments` is present it replaces the base's list
and counter whole; absent, the base's list stays. `stateToJson` therefore
always writes the key, an empty `masks` included, so a written state applied
onto any base gives that state back.

**XMP.** In the arraw namespace, next to the settings, in lower camel case as
every arraw key is:
- `arraw:localAdjustments`, an `rdf:Seq` of `rdf:li rdf:parseType="Resource"`
  structures holding `arraw:id`, `type` (`linear`, `radial`, `brush`), `name`,
  `enabled`, `opacity`, `invert`, the geometry (`fromX`, `fromY`, `toX`,
  `toY`; `centreX`, `centreY`, `radiusX`, `radiusY`, `angle`, `feather`; or
  `rasteriser` and `strokes`), and the non-zero deltas by their table key;
- `arraw:localAdjustmentsVersion`, the list format, 1;
- `arraw:nextLocalAdjustmentId`, the counter.

A write sets all three whenever the list is non-empty or the counter is above
1, and otherwise removes any that are present, so a state with no masks and a
fresh counter leaves no trace and every written state reads back equal.
Numbers are written in shortest decimal (`include/ShortestDecimal.h`), as the
settings are, so they round-trip exactly.

Brush strokes (provisional): `arraw:strokes` is an `rdf:Seq` with one text
string per stroke, `radius hardness flow erase; x,y x,y …`, numbers in
shortest decimal. Base64 binary is the alternative the prototype weighs.
Provisional caps: 10 000 points per stroke and 2 000 strokes per mask. The
edit rules hold the same caps (appending past them throws
`std::invalid_argument`), so a saved list always reads back equal; on reading,
points past the cap are cut from their stroke and strokes past the cap are
dropped, each with a warning.

**Reading.**
- A document without the list loads with no masks.
- Each structure is read in any of RDF's equivalent spellings
  (`rdf:parseType="Resource"`, a nested `rdf:Description`, fields as
  attributes), since other XMP tools may rewrite one as another. Only
  top-level properties of a description are settings (`descriptionsOf`,
  `occurrencesOf` in `src/core/Sidecar.cpp`), so a mask's `arraw:exposure`
  is never read as the global one.
- Dropped with a warning naming the entry (position, id, type and reason):
  an unknown type, a malformed entry, a non-finite number, degenerate
  geometry, an unknown rasteriser version, a duplicate id (the later one) and
  every entry past 16.
- An unknown delta key is dropped from its mask with a warning; the mask stays.
- An out-of-range number is clamped with a warning.
- No unknown mask or field is retained: arraw owns the list and rewrites it
  whole on every write (the Decisions table).
- A list whose version is newer than this arraw's is read as far as it is
  understood, with a warning, and the sidecar is then **not written back**, as
  a newer `arraw:version` already is not (`writeSidecarFor`,
  `src/core/Sidecar.cpp`). Otherwise saving would destroy what a newer arraw
  wrote.
- `readUnknownKeys` skips the three new properties. An arraw from before this
  ADR reports them as unknown settings, renders without masks, and leaves them
  in place when it writes (it sets only the properties it knows).
- Foreign XMP is preserved as today. `sidecarVersion` stays 1: the meaning of
  the settings does not change.
- A sidecar holding the list records a state (`SidecarContents::state`), as one
  holding any settings key does (ADR 039).

**Never copied.** Masks belong to one photograph. No copy section carries them,
and `withLook` (`src/core/Edits.cpp`), which edits `state.settings` through the
descriptor table, leaves the target's list and counter alone. Presets, when
they arrive, are built on `Look` and inherit this.

**Tests.** JSON and XMP round trips of every kind, disabled and inverted
masks, names, opacity, the counter and an empty list with a raised counter,
each reading back equal; every drop and clamp above with its warning; the
counter repair; a newer list version blocking the write and leaving the file
byte-identical; a sidecar written here read by a build without the list
(unknown-key warnings, masks kept on its write; a fixture stands in for the
older build); the alternative RDF spellings; a paste and a look applied onto
a state with masks leave its list and counter alone.

### 10. Front ends

**Command line.** No flags create or change masks; `--exposure` and the like
stay global. `export` renders the masks a sidecar holds. `info` lists each mask
(type, name or default name, enabled, invert, opacity, non-zero deltas), and
`--json` adds the geometry, as `stateToJson` writes it.

**Python.** `DevelopState.local_adjustments` (a tuple of frozen
`LocalAdjustment`, ids included) and the shortcut `photo.local_adjustments`;
`add_linear_mask(from_, to, **deltas)`, `add_radial_mask(centre, radius_x,
radius_y, angle=0, feather=0.5, **deltas)`, `add_brush_mask(strokes,
**deltas)`, each also taking `name`, `opacity`, `invert`, `enabled`, and
appending, so the new mask is `local_adjustments[-1]`; `with_local_adjustment(id,
**changes)`; `without_local_adjustment(id)`. All return new photos; deltas use
the table's Python names. The `.pyi` is regenerated. CPU use needs no
graphics initialisation. A CLI/Python parity test renders one sidecar with
masks through both.

**GUI** (plan section 9, summarised):
- a Masks panel in the develop dock (add, rename, enable, invert, duplicate,
  delete; the selected mask's opacity and delta rows through
  `SettingPresentation`);
- a mask tool, a controller like `CropModeController`, with linear and radial
  handles drawn through view, composition, upright and source;
- a red overlay of the selected mask's coverage (O toggles), from the same
  weight functions as development;
- the brush tool (size, hardness, flow, erase on Alt, `[` and `]`, a cursor
  showing radius and hardness).

Gestures follow ADR 022:
- one handle drag, slider drag or stroke is one begin, update, commit;
- entering crop mode, switching tool or photograph, or losing pointer capture
  cancels a gesture in progress;
- mask and crop modes are exclusive; pan stays on Space.

**History.** Mask edits commit with origin `Edit`. `describeChange` gains a
local part (mask ids added, removed or reordered; per changed mask, its
geometry, strokes, flags or the table keys whose deltas differ). The front end
words "Mask added" or "Linear 2: Exposure +0.50" from it, as ADR 022's
amendment requires, rather than from new origins.

## Consequences

- A mask edit pays the Presence context (when Presence is reachable) and the
  chain, from the Denoise checkpoint; never noise reduction.
- The chain grows by up to 16 weight evaluations and 13 short sums a pixel,
  and, where a mask carries them, an `exp2` for exposure, an `exp2` and a `pow`
  for contrast, and two `exp2` for Temp/Tint. Its cost with 16 masks is
  measured in step 2 on the CPU and in step 3 for slider response; nothing is
  promised before.
- `PresencePlan`, `PixelContext`, `presenceContextFieldsOf` and the GPU's
  Presence inputs change shape (two haze bases). Plans with only global
  Presence keep their pixels.
- `GpuPointwiseBlock` grows from 384 to 2256 bytes, and the pointwise pass
  from six inputs to eleven.
- An empty brush mask with a Dehaze delta still prepares its base: the
  reachability rule is conservative.
- The counter is part of the state: adding a mask and removing it again
  leaves a state that differs from the one before (a real history step), and
  its sidecar carries the counter.
- Local Temp/Tint gives JPEGs a white balance control that the global one
  still refuses them.
- Older arraw builds warn about the new properties, render without masks and
  keep the list when saving; a newer list blocks writing in this one.
- Brush memory is real: 24 MB per full-size brush mask at 24 MP before the
  prototype's resolution decision.
- The plan step 4 prototype amends sections 6, 7 (packing) and 9 (stroke
  encoding and caps).

## Alternatives considered

- **Sequential per-mask chains** (each mask runs its own tone and colour stages
  on the result of the previous). Order-dependent, 16 times the chain for 16
  masks, and an exposure under a contrast under another exposure no longer has
  ADR 013's monotonicity argument. Summing in setting units is commutative,
  clamps once and costs one chain.
- **A weight context pass** (rasterise every mask's weight into a side image
  the chain reads, as Presence's context). Simple shaders, but 16 weights a
  pixel is 64 bytes at float, a pass of its own, and a parametric shape
  quantised to a raster. Linear and radial masks are a few multiplies
  analytically and exact at every level; only brushes need textures.
- **Raster brush storage** (store the painted coverage). Resolution-bound,
  megabytes per mask in a sidecar, and a preview-resolution stroke would be
  blurry in an export. Strokes are small, re-rasterise at any level, and
  ADR 009's frame rule applies to their points.
- **Opaque retention of unknown masks** (keep XML a newer arraw wrote and write
  it back). It cannot render them, ids and order would mix with masks it does
  own, and the user chose drop with a warning (Decisions table). Refusing to
  write a newer list covers the dangerous case.
- **One-sign Dehaze** (one haze base chosen by the global sign, or refusing
  masks whose sign differs). A negative amount in the floor's formula,
  `1 - dehaze * 0.6 * s * open`, scales the colour up rather than adding a veil, and refusing a negative Dehaze mask
  under a positive global would be a trap. Two bases, only when both signs are
  reachable, cost two extra renders.
- **The plan's symmetric two-constant Temp/Tint law**
  (`log2 g = (+aT - bN/2, bN, -aT - bN/2)`). Its best fit is `a = 0.30`,
  `b = 0.165` (with the Tint sign reversed: as written, positive Tint would
  have gone green, opposite to the global Tint). But a kelvin step moves blue
  2.6 times as far as red against green, and the symmetric law misses that by
  0.13 stops a channel at ±100: warming would read orange-red rather than
  yellow. The two-vector law costs the same.
- **A second uniform block.** It would keep `GpuPointwiseBlock` as it is, but
  `GpuContext` binds one block per pass at binding 1, so every pass would
  change for one.
- **New `EditOrigin` values for mask edits.** ADR 022 words steps from the
  states; a description of the local change keeps that rule.
