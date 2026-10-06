# Texture, Clarity and Dehaze read a context of log luminance

Texture, Clarity and Dehaze change a pixel by how it stands against its
surroundings, so each needs a blurred neighbourhood; but what they change is
tone, after Basic Tone and before the curves, which is inside the pointwise
chain. This ADR records how the chain gets the neighbourhood without becoming
two passes, the maths of the three controls, where they sit, and how previews,
regions, checkpoints and the GPU treat them. It implements the Texture,
Clarity and Dehaze part of D4 of the
[global adjustments plan](../ideas/global-adjustments-plan.md), option (a): a
context side-product read by the fused chain. Dehaze subtracts a share of an
airlight-like floor estimated from the coarse grid, a dark channel in log
luminance, with `main`'s restrained colour (its ADR 0011); the first version
took `main`'s veil-as-a-share-of-the-local-mean, which a review showed to be
nearly inert on haze at the scale haze has (see the rejected options).

## Decision

**Settings.** `PresenceSettings` (`include/PresenceSettings.h`),
`DevelopSettings::presence`:

| Key | Range | Default | `main` / Lightroom |
|---|---|---|---|
| `texture` | -100 to 100 | 0 | Texture (`crs:Texture`) |
| `clarity` | -100 to 100 | 0 | Clarity (`crs:Clarity2012`) |
| `dehaze` | -100 to 100 | 0 | Dehaze (`crs:Dehaze`) |

`main`'s ranges, bipolar. The rows are in a new `SettingGroup::Presence`,
`Stage::Pointwise`, applicable to every photograph. Lightroom shows them in its
Basic panel under Presence, with Vibrance and Saturation; the develop panel
shows a **Presence** group of the three directly under Tone, in both
treatments. Vibrance and Saturation stay in the Colour group, which Black &
White hides (ADR 027), rather than moving rows that already have a home.
Non-finite values are refused and out-of-range ones clamped (ADR 008). The
command line takes `--texture`, `--clarity`, `--dehaze`; Python has
`PresenceSettings`, `DevelopSettings.presence`, `SettingGroup.PRESENCE` and the
flat keys.

**Placement: after `shapeTone`, before the tone curves, and so inside the curve
input tap.** `developToCurveInput` is now matrix, exposure, `shapeTone`, then
`applyPresence`. ADR 035's definition of the tap, "after Basic Tone,
immediately before the curves", holds as written: Lightroom counts Presence as
Basic, and the histogram behind its curve shows the image with Clarity and
Texture. The shoulder still follows the curves and catches whatever Presence
lifts past white. `curveInputFieldsOf` includes the Presence block, so
`sameAtTap` (and the GUI's histogram) refreshes after a Presence edit; the GPU
tap is still the `AfterTone` probe, which now stops after Presence.

**The context is a side image of the pointwise pass's input, recomputed with
the pass.** It holds up to four grids of log2 luminance. Each *base* is a
box-reduced grid, optionally opened by a window, then blurred by a normalised
separable Gaussian (across, then down, edges clamped):

- the *fine* base, for Texture, when Texture is not zero;
- the *coarse* base, for Clarity, when Clarity is not zero, together with the
  coarse grid *unblurred* (the *coarse cells*), the top of Clarity's band,
  kept also for a positive Dehaze, which measures its floor's share against
  them;
- the *haze* base, for Dehaze, when Dehaze is not zero, on the same coarse
  cells: for a positive Dehaze their *floor*, the opening of the cells by a
  window (the minimum over it across and down, then the maximum of that across
  and down), reconstructed towards the cells (`reconstruction` steps, each the
  maximum over a cell's 3x3 neighbourhood, then the minimum of that and the
  cell), under a light blur that never lowers a cell below the opening
  (`floor = max(opened, blurred)`), read against the coarse cells unblurred;
  for a negative one their *mean*, a broad blur and no window.

Clarity and Dehaze share one reduction to the coarse cells. A cell is the log2
of the mean luminance of the `reduction x reduction` block it covers (the last
row and column average what they have), each luminance through the source's
**as-shot row** (`asShotLuminanceRow`, shared with the Denoise pass, ADR 039)
and bounded to `[2^-14, 2^16]` (`boundedLuminance`: NaN and below to the floor,
infinity and above to the ceiling), so that a non-finite pixel moves its cell
by a bounded amount instead of poisoning its neighbourhood. The input is the
source after noise reduction, so the context reads the denoised luminance.
Plain Gaussian bases, as `main`'s; an edge-aware base (the guided filter, once
the Denoise seam has one) is the planned refinement for halos at strong
Clarity.

**The plan.** `PresencePlan` (`src/core/Presence.h`) is a field of the plan in
the *pointwise* group of `stagesOf`: the amounts (minus one to one), the
as-shot row, and each base's `reduction`, `sigma`, `radius`, `window` and `reconstruction` in
source pixels and cells. Every control at zero resolves to the default block
and the chain reads no context. The context is a function of the pixels at
the Denoise boundary and of `presenceContextFieldsOf` (row and the three
bases) alone:
- white balance and exposure never reach it, structurally: the row is as
  shot and both are later in the chain;
- the amounts do not reach it either, only whether each base exists and
  Dehaze's sign (floor or mean);
- noise reduction does, through the pixels, and the radii through the source's
  size and pixel scale.

The block sits in the pointwise group because the context is consumed, and
recomputed, by the pointwise pass. A Presence edit therefore invalidates the
pointwise checkpoint and nothing earlier: it resumes from the Denoise
checkpoint, as a tone edit does.

**Maths: detail in log2 luminance, applied hue-preserving.** For a pixel with
source colour `s` (before white balance), `L = log2(boundedLuminance(row . s))`.
The grids are read at the pixel (below): `fine`, `coarse`, `cells` (the coarse
cells unblurred) and `haze`. `softLimit(d, k) = d / (1 + |d| / k)` has slope
one at zero and never exceeds `k`. With `Y` the luminance of the toned colour
`c` and `v` its perceptual value `clamp(Y, 0, 1)^(1/2.2)`:

```text
stops = texture * softLimit(L - fine, 0.5)
      + clarity * midtones(v) * softLimit(cells - coarse, 1)
c    *= 2^stops                                  (hue and saturation kept)
midtones(v) = smoothstep(0, 0.8, 1 - |2v - 1|)   (main's weight)
```

*Texture* is the pixel against a small blur: everything finer than about four
sensor pixels. *Clarity* is a **band**, the coarse cells against their blur:
detail between a cell (8 sensor pixels on a 24 MP frame) and the coarse sigma
(60). Detail finer than a cell, Texture's and the noise, is averaged out of
the cells and so out of Clarity: Clarity -100 lowers mid-scale contrast
without blurring texture, and Clarity +100 does not sharpen what Texture
owns. On a small source whose coarse cell is a single pixel (long edge below
800 sensor pixels) the band reaches down to the pixel, as before.

*Dehaze* then works on `Y' = Y * 2^stops`, sparing what is nearly white with
`open = 1 - smoothstep(0.75, 1.25, Y')`:
- a positive Dehaze takes off a share of the floor. `s = 2^min(haze - cells,
  0)` is the floor's share of the coarse cells at the pixel (their bilinear
  read, the grid Clarity's band reads), not of the pixel: one at or below the
  floor, less above it. The colour is scaled by `1 - dehaze * 0.6 * s * open`,
  which for a cell above the floor is subtracting `0.6 * dehaze` times the
  floor (in the cell's own scale), and for one at or below it keeps 40% of
  every pixel in it. The result never crosses black, and subtracting an offset
  raises contrast at every scale above a cell inside the veil: between
  textures and between regions larger than the coarse sigma. Detail finer than
  a cell (8 sensor pixels on a 24 MP frame), and noise, is scaled by a factor
  that varies smoothly across the cells, so its contrast is kept rather than
  expanded;
- a negative Dehaze adds the neutral veil `|dehaze| * 0.4 * open * m`, with
  `m = Y' * 2^min(haze - L, 6)` the surroundings' mean in the colour's own
  scale (a pixel more than six stops below them gets the veil of one six
  stops below);
- then Saturation (ADR 027's Oklab chroma scale) of `dehaze * 0.16` times the
  veil's share, `s * open` or `V / (Y' + V)`.

Every ratio Dehaze takes is of as-shot logarithms, so a change of exposure
scales its result and changes nothing else (tested in neutral tone, both
signs); only `open` and Clarity's `midtones` read the toned value, as the
photographer's judgement of "white" and "midtone" does. The floor is the
dark-channel assumption: a window of 3% of the long edge almost always holds
something dark, so the darkest cell found there is the veil over it. Opening
rather than the plain minimum keeps a region wider than the window on its own
floor up to its edges, so a broad flat area (a sky, a wall) is not left with a
bright rim of less Dehaze inside its border. The opening's square window
reaches the edge only where it fits flush, so on its own it left a curved edge
(a disk, a rounded cloud, the tip of an oblique corner) with bright crescents
of less Dehaze on its axis-facing sides, about `w^2 / 2R` wide (22 pixels and
up to 1.2 stops on a disk of 150 pixels at 2400): a square imprint. So the
opening is then **reconstructed**: each step takes the maximum over a cell's
3x3 neighbourhood and the minimum of that and the cell unopened, which brings
an opened bright area back by one cell towards its own outline and never above
the cells. The count is fixed, the window's radius in cells (23 at 24 MP, 18
at 2400 pixels), so the device renders as many steps every time; iterating
until stable, measured at the production cells, a disk needs 7, 4 and 2 steps
at radii of 375, 750 and 1500 pixels at 6000 (2400: 5, 3 and 2 at 150, 300 and
600), and the smallest disk that holds the window, `R = w * sqrt(2)`, needs 10,
about `0.42 w`: the count covers every disk with room to spare, and the
narrow ends of other shapes up to a window's radius deep. Min and max are
exact, so both backends agree bit for bit on it. The light blur (a quarter of a
percent of the long edge, about two cells) hides the cells' steps, and is
never allowed to lower a cell below its opening, so it cannot carry a dark
neighbour's floor across an edge either. On a step from 0.03 to 0.3, both
sides wider than the window (1200 pixels, the edge off the cells' grid),
Dehaze 100 takes -1.322 stops off every pixel of the bright side up to the
edge, and off the dark side: no rim (tested to 0.01 stop; measured 0.000).
With the floor's blur free to lower it, and `s` measured against the pixel,
the bright side kept 0.91 stop more at the edge, over a 7-pixel rim at 1200
pixels (the review measured 1.0 stop over 35 pixels at 6000). On disks of
150, 300 and 600 pixels (0.3 on 0.03 under a uniform veil, 2400 pixels), Dehaze
100 takes the centre's -1.322 stops off every pixel up to the edge, on the
axis-facing sides and on the diagonals alike (tested to 0.02 stop; measured
0.0000), where the opening alone kept up to 0.44 stop more at the edge of the
600-pixel disk and 0.64 at that of the 150-pixel one; the light ring just
outside the edge is gone too.

Known behaviour, as Lightroom's: a broad *flat* area at any tone is all
floor (`s = 1`), so Dehaze 100 darkens it by up to 1.3 stops (a pure
exposure change), more than a textured area of the same brightness, whose
lighter parts stand above their floor; and the darkest parts of any area
lose up to 60% of themselves. Because `s` is the cell's, noise and fine detail
in a flat area are not expanded: on flat mid grey (3400 pixels, cells of 8),
Dehaze 100 takes a quarter-stop noise to 1.023 and a 5-pixel modulation to
1.006, against 1.725 and 1.704 for Texture 100 (with `s` measured against the
pixel they went to 1.760 and 1.752, more than Texture). The price is that
the veil comes off only above a cell's scale: a dark speck finer than a cell
inside a hazy area is scaled with its cell, not deepened. A colour with no luminance (at or
below `liftedBlackThreshold`, or NaN) is returned as it is. All constants are
in `Presence.h`. Since Texture's and Clarity's gain is `2^stops` with
`|stops| <= |texture| * 0.5 + |clarity| * 1`, **a pixel moves by at most
`|Clarity| / 100` stops for Clarity alone, whatever the edge** — the halo
bound, tested on a two-stop and an eight-stop step.

Measured on the tests' synthetic images (log2-luminance spread, after over
before, unless stated):
- Texture 100 multiplies a 4-pixel modulation by 1.87, a 256-pixel one by
  1.005; Texture -100 takes the fine one to 0.13 and leaves the broad at 0.995.
- Clarity 100 and -100 take a mid-scale pattern to 1.62 and 0.39 (a 320-pixel
  source, cells of one pixel); on a two-stop step, Clarity 100 moves the
  pixels beside the edge by -0.33 and +0.47 stops, and nothing beyond five
  sigmas. On a 3400-pixel source (cells of 8), a 5-pixel modulation goes to
  1.002 at Clarity 100 (Texture 100: 1.86) and 1.014 at -100, while a
  128-pixel one goes to 1.69 and 0.31.
- Dehaze 100 and -100 take a fine veiled pattern to 1.96 and 0.71. On a
  broad veil (40-pixel blocks, 0.02 and 0.3, under a uniform 0.3 veil over the
  lower half of 1200 x 800), the contrast between light and dark blocks under
  the veil goes from 1.54 to 2.33 at 100 (2.34 before the reconstruction) and
  1.37 at -100, the clear half against the veiled one from 0.63 to 0.98, and
  nothing crosses zero. On flat mid grey at 3400 pixels, Dehaze 100 takes a
  quarter-stop 128-pixel modulation (16 cells) to 2.03, noise to 1.022 and a
  5-pixel one to 1.006 (tested to at most 1.2).

**Radii.**
- *Texture* is a detail control, in sensor pixels: sigma 4 sensor pixels on a
  grid of 2-sensor-pixel cells. On a reduced source the cell is
  `bit_floor(clamp(floor(2 / scale), 1, 2))` source pixels and the sigma
  `4 / scale / cell` cells, held at **at least one cell**: a deep preview level
  shows Texture at the finest scale it has, coarser than at 1:1, rather than
  none. The preview approximates; 1:1 is where Texture is judged, as noise
  reduction.
- *Clarity and Dehaze* are composition controls, relative to the uncropped
  long edge: Clarity's sigma is 1% of the long edge in sensor pixels (60 on a
  6000-pixel frame); the sensor cell, shared with Dehaze, is the largest power
  of two leaving four cells a sigma (8 on 6000 pixels: sigma 7.5 cells, radius
  23). Dehaze's floor is opened by a window of radius 3% (23 cells),
  reconstructed by 23 steps and blurred by a sigma of 0.25% (1.875 cells,
  radius 6); its mean, for a
  negative Dehaze, by a sigma of 2% (15 cells, radius 45). On a reduced source
  the cell is `bit_floor(clamp(floor(cellSensor / scale), 1, cellSensor))`, so
  a pyramid level covers the same sensor pixels per cell as the source while
  it can, and the sigma and window in cells are the same. A level-2 preview of
  a scene with Clarity 80 and Dehaze 40 differs from the full render reduced
  afterwards by 0.1% of the effect (test bound 2%).
- Sigmas and windows are at most a radius of 64 cells
  (`maximumPresenceRadius`), the shader's bound. The crop does not enter: a crop edit never recomputes the
  context.

**The chain reads the context bilinearly at the pixel's coordinate.** As the
Denoise pass reads its grid: `u = (x + 0.5) / reduction - 0.5`, clamped to the
grid, rows first (`gridTap`, now shared in `src/core/ReducedGrid.h`), exact in
float because the reduction is a power of two. On the CPU,
`PresenceSampler` precomputes the fine and the coarse grids' column taps (the
three coarse grids share them) and `developSamples` passes a
`PixelContext{fineBase, coarseBase, coarseCell, hazeBase}` to
`developPixel(plan, colour, context)`: ADR 011's context, arriving with its
first reader. The context has **no default**: a zero `PixelContext` is not
"no context" but every base at log2 = 0, which would move each pixel by
nearly the full limit. Callers without one use `developPixel(plan, colour)`,
which asserts that the plan has Presence off.

**The CPU chain now runs on threads.** Presence adds a `log2`, an `exp2` and
two bilinear reads a pixel, and Dehaze an Oklab round trip, so the pointwise
pass was split over `forEachRowBand` (ADR 039). Each pixel reads only its own
colour and the context, so the result is the single-threaded one bit for bit
(tested). The context's reduction and blurs are banded the same way.

**The GPU computes each grid in a few renders and binds them to the pointwise
pass.** `GpuPass::PresenceFilter` (`presence_filter.frag`, one std140
`GpuPresenceBlock` of 320 bytes with the opening's `window`, `PresenceStep`
Reduce, BlurAcross, BlurDown, MinimumAcross, MinimumDown, MaximumAcross,
MaximumDown, BlurDownAboveOpening, Reconstruct) writes one-channel R32F targets where the device has them (ADR
039's fallback otherwise). The weights are the host's `denoiseWeights`,
uploaded; a minimum and a maximum are exact on both. `GpuPass::Pointwise` now
takes six inputs, the image, the curves, and the fine base, coarse base,
coarse cells and haze base at bindings 3 to 6, the image standing in for a
grid that is off, as for the curves. `GpuPointwiseBlock` grows to 384 bytes:
`presenceLumaRow`, `presence`, `textureAmount`, `clarityAmount`,
`dehazeAmount` (not `texture`, which is a GLSL built-in), and the fine and the
coarse grids' reductions and sizes (the three coarse grids share theirs).
`develop.frag`'s `applyPresence` mirrors the C++ line for line. Texture adds
three renders (reduce, blur across and down); Clarity three; negative Dehaze
three and positive Dehaze seven plus its `reconstruction` steps (the four
of the opening, then one render a reconstruction step, which also reads the
cells at binding 2 and keeps below them; the last blur down reads the opened
grid there and keeps above it, so `presence_filter.frag` takes two inputs,
the first standing in for the second in every other step): 30 renders at 24
MP, 25 at 2400 pixels. One fewer each when they share the reduction with
Clarity: all three positive add twelve plus the steps, and every control at
zero none.

**Checkpoints: recompute, do not cache.** Release build, 6000 x 4000
RgbaF32, best of three, 8 threads (the measurement machine has 8):

| case | context alone | pointwise boundary |
|---|---|---|
| no Presence | - | 0.31 s |
| Texture | 0.10 s | 0.53 s |
| Clarity | 0.03 s | 0.59 s |
| all three | 0.14 s | 1.11 s |

(Before threading the chain the boundary was 0.71, 1.34, 2.26 and 4.27 s.)

After the airlight floor and the Clarity band (RgbU8 PNG export, best of
three, a busier machine, so the baseline is higher): no Presence 0.39 s;
Texture 0.13 / 0.59 s; Clarity 0.04 / 0.66 s; Dehaze 50 0.05 / 0.87 s;
Dehaze -50 0.06 / 0.90 s; all three 0.20 / 1.27 s. The opening adds little
to the context (its grid is 750 x 500); Dehaze's cost is in the chain, an
Oklab round trip on nearly every pixel for its colour.

After the reconstruction (6000 x 4000 RgbU8 PNG, CPU, release, best of five,
8 threads, a loaded machine; the same binary built with and without the
steps, run alternately, two rounds): Dehaze 50, context 63 / 86 ms without
and 84 / 101 ms with the 23 steps, about 20 ms more; the pointwise boundary
1.07-1.24 s either way, the difference inside the run-to-run noise (no
Presence 0.38-0.41 s; all three 0.24-0.27 s context, 1.55-1.74 s boundary
without, 0.26-0.29 s and 1.58-1.71 s with). On the GPU it is 23 more renders
of a 750 x 500 one-channel grid, nine taps each; only lavapipe was at hand,
where the whole develop went from 0.83-0.86 s to 0.95 s, about 4 ms a
render on a software rasteriser. Not measured on a hardware GPU.

The context is at most a sixth of the pass it feeds, and less than the
Denoise colour half every RAW runs (0.40 s, ADR 039). Caching it as an
auxiliary image on the `Denoise` checkpoint would save that share on a tone
tick, at the price of a second payload in `RenderCheckpoint` on both backends
and of the preview holding it. It is not worth it yet. If it becomes so, the
cache's validity is `presenceContextFieldsOf` equality on top of the Denoise
prefix, and `presenceContextFieldsOf` is already the grouping to compare.

**Regions.** The region is cut after geometry and the context is made from
the whole source, so a region with Presence is the crop of the whole render,
bit for bit (tested, with noise reduction on). When the earlier stages are
restricted to a region's footprint, `presenceReach(plan)` is the margin in
source pixels each base needs: `(2 * window + reconstruction + radius + 2) *
reduction`, the larger of the two (the opening's minimum and maximum reach a
window each, and each reconstruction step a cell; before the reconstruction
the formula counted the window once, which undercounted the opening).

**Tolerance.** The GPU's pointwise boundary with Presence agrees with the CPU
to 3.9e-6 relative on lavapipe (each control alone and together, both signs,
pixel scales 1 to 8, odd sizes with partial cells, a wide source with 1-pixel
and multi-pixel cells, an export-sized long edge of 3203 with 8-pixel cells
and a 23-cell opening, a bright disk whose opening the 18 reconstruction
steps bring back to its outline, a source with an infinite and a NaN pixel, the camera
fixture's as-shot row). It is held to the
pointwise tolerance, 3e-5: what is the device's own is `log2` and `exp2` of
values a few stops in size and float sums; a wrong cell, tap or bilinear
weight disagrees by 1e-3 or more. The curve-input sample and an end-to-end
render with denoise, straighten and a resize agree within the same and the
resample tolerances. Only lavapipe was measured.

## Consequences

- Presence edits resume from the Denoise checkpoint on both backends and pay
  the context (reduced resolution) plus the chain; a white balance or exposure
  edit pays the same, as the context is recomputed rather than kept.
- The curve histogram follows Presence edits: a Clarity drag resamples it on
  each settle, as a Basic Tone drag does.
- The CPU pointwise pass is threaded for every render, Presence or not:
  0.71 s to 0.31 s at 24 MP with no Presence.
- `planFor(encoding, state, pixelScale)`, which knows no size, leaves Presence
  off; the buffer and Photo overloads resolve it. Tests that build a plan from
  an encoding alone never see it.
- Halos at strong Clarity are bounded but visible on hard edges (about half a
  stop beside a two-stop step at 100); the edge-aware base is the way to lower
  them, behind the same plan block. Dehaze's floor would gain from the same
  edge-aware filter. At a straight step it leaves no rim, and after the
  reconstruction no curved edge or corner of a bright area that holds the
  window does either (measured above). What the reconstruction cannot fix is
  a bright area that holds no window-sized square anywhere (narrower than
  the window's side, `2w + 1` = 47 cells or 376 pixels at 24 MP, about 6% of
  the long edge, in its widest part): it has nothing to grow back from,
  so its floor is its darker surroundings', and it gets less Dehaze than a
  broad area of its tone, as before. A part of a large area more than 23
  cells (a window's radius) from where the window fits, the far end of a
  long narrow tongue, is likewise only brought back that far.
- The reconstruction's own price: it follows every 8-connected path of cells
  at or above a level, so it also climbs from a bright area into a noisy or
  finely textured neighbour, through the brighter cells of the texture, up to
  23 cells (a window's radius) deep. There the floor rises towards the
  texture's own cells, so Dehaze treats more of it as floor: a darker band of
  more removal, about a window's radius wide (36 pixels at 1200), beside a
  bright area's edge on the textured side, square-cornered around a soft
  bright blob (the 3x3 step spreads as a square), and a little more removal
  over noisy areas generally. On the phase-7 review scene (1200 x 800,
  Dehaze 100) the noisy half's mean removal went from about -1.0 to -1.2
  stops, -1.3 in the band beside the bright rectangle (-1.1 further out), and
  the textured lower half from -1.08 to -1.17, -1.3 in a band under the
  rectangle; the texture's log spread rose a little (0.232 to 0.263), noise
  not (0.542 to 0.551). The flat-noise test (a 16-pixel strip) does not see
  it.
- Dehaze 100 darkens broad flat areas, skies above all, by up to 1.3 stops,
  as Lightroom's does. It removes the veil only above a cell's scale (8
  sensor pixels at 24 MP); finer detail and noise keep their contrast.
  Release timing with the reconstruction is measured above.
- Tests: defaults resolve to the default block with no context read and the
  same pixels, and no GPU render; white balance, exposure and the amounts leave
  the context's fields and its cells unchanged; Clarity on a step raises local
  contrast, moves nothing beyond five sigmas and no pixel beyond the bound, on
  a two-stop and an eight-stop edge, both signs; Texture moves fine detail and
  spares broad shapes, both signs; negative Clarity softens; Clarity on 8-pixel
  cells leaves fine detail alone at both signs and keeps its mid-scale effect;
  Dehaze raises a finely veiled image's contrast, deepens its blacks without
  crossing zero and adds colour, and negative Dehaze does the reverse; Dehaze
  takes off a broad veil over blocks above Clarity's sigma (block contrast
  1.54 to more than 1.4 times that, the veiled half darkening more than the
  clear) with every channel above zero; Dehaze 100 takes the same off a bright
  area up to its edge as inside it, within 0.01 stop, and the dark side too,
  and off disks of 150, 300 and 600 pixels under a veil up to their edges on
  the axis-facing sides and the diagonals, within 0.02 stop of the centre;
  on flat mid grey it raises noise and a 5-pixel pattern no more than
  Texture 100, and no more than 1.2 times; an infinite pixel under negative Dehaze is not NaN; Dehaze at both signs is the same at
  every exposure; a non-finite pixel leaves its neighbours finite and the far
  pixels as they were; black stays black and the hue is kept without Dehaze; a level-2 preview within 2% of the full render's
  effect; a region is the crop of the whole render; Presence, exposure and
  white balance edits resume from Denoise bit for bit, a crop from Pointwise,
  and a Presence edit is refused at Pointwise; the curve input includes
  Presence and a curve edit still leaves it alone; the threaded context and
  chain equal one thread; non-finite values are refused; GPU parity, pass
  counts, a resume from the GPU's Denoise checkpoint bit for bit and the GPU
  tap, at production cell sizes and with non-finite pixels; the panel's
  Presence group in both treatments; CLI and Python parity.

## What was rejected, and why

- **A spatial pass on toned values, splitting the chain in two** (D4 (b)).
  Truer to "local contrast of what you see", but every tone slider would pay
  for a spatial pass, and the pointwise checkpoint would sit in the middle of
  the chain.
- **The tap before Presence.** It would keep the curve histogram still while
  Clarity moves, but Lightroom's curve histogram includes Presence and ADR
  035's words, "after Basic Tone", would then need an exception.
- **The context block in the denoise group.** Turning Texture on would then
  invalidate the most expensive checkpoint.
- **Detail measured on the white-balanced, exposed luminance.** Every white
  balance or exposure tick would change the detail, and with it the context,
  which could then not be shared; the as-shot row keeps both out.
- **The mean of log luminance per cell.** Exact for the same invariance, but
  noisier near black; the log of the mean weighs dark pixels as their light
  does, as the Denoise grid does.
- **The whole coarse detail (`L - coarse`) as Clarity's.** The first version
  did this: an unsharp mask of every scale below the coarse sigma, so Clarity
  100 sharpened fine detail more than Texture 100 (log spread 1.49 against
  1.42 on a review image) and Clarity -100 blurred texture and noise away.
- **The band `fine - coarse`.** Also keeps Clarity off fine detail, but costs
  the fine base (0.10 s at 24 MP on the CPU, three GPU renders) whenever
  Clarity is on; the coarse cells are already computed.
- **A veil as a share of the coarse local mean** (the first version: `V = 0.2
  * w * m`, the colour scaled by `Y / (Y + V)`, with `main`'s haze weight `w`
  on linear values). Where the local mean equals the pixel, at any scale above
  the coarse sigma, that is a pure scale: no contrast gain between regions,
  which is where haze is. Since `w` grew with the mean, brighter flat regions
  lost more and the contrast between regions even fell (1.345 to 1.329 on a
  review image); on linear mid-grey `w` was 0.09, so Dehaze 100 removed about
  2%. Its local contrast term (`0.5 * softLimit(L - coarse, 1)`) was half a
  Clarity. A veil proportional to the pixel itself is the same scale at every
  scale; `main`'s absolute veil weighted up in bright areas (`-0.08 * veil`)
  compresses bright regions more than dark ones.
- **The plain minimum as the floor, under a broad blur.** A flat bright area
  wider than the window is all floor in its interior, and the minimum carries
  the darker floor of its surroundings a window plus the blur into it: a
  bright rim of less Dehaze inside its edge, 60 pixels on the review image.
  The opening keeps the area's own floor up to its edge.
- **The opening alone, with its square window** (before the
  reconstruction). It left the square imprint inside round bright areas
  described above. A rounder window (the minimum and maximum along the
  diagonals too, an octagon) would shrink the crescents to about a fifth for
  four more renders but not remove them; an edge-aware refinement of the
  floor is future work.
- **The floor's blur free to lower it.** It carried a dark area's floor about
  three sigmas into a bright neighbour: up to 1 stop less Dehaze at the edge,
  fading over 0.6% of the long edge. Keeping the blur above the opening is one
  `max`; shrinking the blur instead would shrink the rim but show the cells'
  steps on smooth veil gradients, and an edge-aware refinement is future work.
- **The floor's share measured against the pixel, `s = 2^min(haze - L, 0)`.**
  The exact subtraction at every scale, but where the floor is close to the
  local mean (any flat or finely textured area) the fluctuations above the
  floor were expanded about 2.5 times and those below it scaled by 0.4: Dehaze
  100 raised noise 1.76 times and a 5-pixel pattern 1.75 times on flat mid
  grey, more than Texture 100, and lopsidedly (bright peaks grew, dark ones
  did not).
- **Removing the floor by `Y / (Y + V)`.** It never crosses black, but takes
  half of a pixel at the floor where the subtraction it stands for takes all
  of it, and is not an offset above it; `1 - k * s` with `s` capped at one is
  an exact subtraction above the floor, a fixed share below it.
- **An airlight in the dark-channel paper's sense, `J = (I - A(1 - t)) / t`.**
  The division by `t` brightens hazy regions back, which needs a global
  airlight colour and a transmission refined by an edge-aware filter; the
  subtraction alone is the part that raises contrast, and darkening is what
  Lightroom's Dehaze does.
- **Texture vanishing in deep previews.** A sigma below a pixel of the level
  does nearly nothing, so dragging Texture at fit-to-window would show no
  change; held at one cell, the preview shows it coarser instead.
- **Caching the context on the Denoise checkpoint now.** See the measurement:
  a sixth of the pass at most, for a second payload on both backends.
