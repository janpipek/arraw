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
  regular octagon (the minimum over it, across, down and along both
  diagonals, then the maximum of that along the same four), reconstructed
  towards the cells by two steps (each the maximum over a cell's 3x3
  neighbourhood, then the minimum of that and the cell), under a light blur
  that never lowers a cell below the opening (`floor = max(opened,
  blurred)`), read against the coarse cells unblurred;
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
bright rim of less Dehaze inside its border.

**The window is a regular octagon, and the opening is reconstructed by two
steps.** A square window reaches a curved edge only where it fits flush, so
on its own it left a curved edge (a disk, a rounded cloud, the tip of an
oblique corner) with bright crescents of less Dehaze on its axis-facing
sides, about `w^2 / 2R` wide (22 pixels and up to 1.2 stops on a disk of 150
pixels at 2400): a square imprint. Reconstructing that opening by as many 3x3
steps as the window's radius removed the imprint but climbed, through the
brighter cells of any texture beside a bright area, up to a window's radius
deep into it: a darker, flatter band beside every bright edge, square-cornered
around a soft highlight (below). The octagon is a rounder window made of 1-D
passes, each exact: the minimum across and down over a half-width `a`, then
along the diagonal and the antidiagonal over `n` steps (a step is one cell
across and one down), then the maximum along the same four, in the same
order. A square of half-width `a` widened by two diagonal segments of `n`
reaches `a + 2n` along the axes and `sqrt(2) (a + n)` along the diagonals; it
is regular when `a = sqrt(2) n`. For a window of inradius `w`,
`n = round(w (1 - 1/sqrt(2)))` (about 0.29 w) and `a = w - 2n` (about
0.41 w): the axis inradius is exactly `w`, as the square's was, and the
diagonal one within a cell of it (`octagonOf`; 23 gives `a = 9, n = 7` and a
diagonal inradius of 22.6, 18 gives 8, 5 and 18.4). A diagonal pass reads
only the cells of its own parity (`x + y` even or odd about the centre), so
`n` is held to at most `(w - 1) / 2`, keeping `a` at least one: the passes
across and down mix the parities, and a window of one or two cells is a
square. Tested on a checkerboard of single cells, whose floor is the dark
cells' everywhere for windows of 1 to 64 (diagonal passes alone would find
each light cell its own floor), and on the device. The octagon's corners
reach `w / cos(22.5°)`, 1.08 w, so it misses a curved edge by at most about
0.08 w between them (1.9 cells at 24 MP); **two reconstruction steps** close
that. On the cells, every disk that holds the octagon, from the smallest to
four windows in radius at three sub-cell offsets, is reconstructed exactly
for every window from 3 to 23 cells (one step leaves up to 3 stops on the
smallest disks from 16 cells up; at 24 cells, two steps leave at most 0.04
stop of one cell on a few disks of 28 to 30 cells; at 30, three would be
needed). The window is at most 24 cells at full resolution and on
power-of-two levels: the cell is the largest power of two below a quarter of
a percent of the long edge, so the 3% window is 12 to 24 cells. The steps are
a fixed count, so the device renders as many every time; min and max are
exact, so both backends agree bit for bit on the opened grid.

The light blur (a quarter of a percent of the long edge, about two cells)
hides the cells' steps, and is never allowed to lower a cell below its
opening, so it cannot carry a dark neighbour's floor across an edge either. On
a step from 0.03 to 0.3, both sides wider than the window (1200 pixels, the
edge off the cells' grid), Dehaze 100 takes -1.322 stops off every pixel of
the bright side up to the edge, and off the dark side: no rim (tested to 0.01
stop; measured 0.000). With the floor's blur free to lower it, and `s`
measured against the pixel, the bright side kept 0.91 stop more at the edge,
over a 7-pixel rim at 1200 pixels (the review measured 1.0 stop over 35 pixels
at 6000). On disks of 90, 150, 300 and 600 pixels (0.3 on 0.03 under a uniform
veil, 2400 pixels, window 72 pixels), Dehaze 100 takes the centre's -1.322
stops off every pixel up to the edge, on the axis-facing sides and on the
diagonals alike (tested to 0.02 stop; measured 0.0000), where the square
opening alone kept up to 0.44 stop more at the edge of the 600-pixel disk and
0.64 at that of the 150-pixel one, and the octagon alone 0.21 at that of the
600-pixel one; with one step the 90-pixel disk kept 0.33. The light ring just
outside the edge is gone too.

**Beside a bright area, a texture keeps its own floor** (the review's R4,
measured end to end, Dehaze 100, CPU). On the test scene (3400 x 1200, cells
of 8, window 13 cells: a dark blotchy texture about a stop from end to end at
a scale of a cell and a half, a bright rectangle on its right and a Gaussian
blob of 120 pixels on its left, under a uniform veil), the extra removal
beside the rectangle's edge, against the texture far from both, at 0, 8, 16,
24, 32, 40 and 48 pixels from it:

| floor | 0 | 8 | 16 | 24 | 32 | 40 | 48 | mean 24-120 | ring around the blob | its corners |
|---|---|---|---|---|---|---|---|---|---|---|
| square + 13 steps (before) | -0.31 | -0.30 | -0.29 | -0.28 | -0.28 | -0.28 | -0.24 | -0.24 | 0.26 | 0.12 |
| octagon, no steps | | | | | | | | -0.01 | 0.07 | 0.03 |
| octagon + 1 step | | | | | | | | -0.02 | 0.09 | 0.03 |
| **octagon + 2 steps** | -0.31 | -0.28 | -0.18 | -0.12 | -0.11 | -0.06 | 0.00 | **-0.03** | **0.11** | **0.03** |
| octagon + 3 steps | | | | | | | | -0.05 | 0.13 | 0.03 |
| octagon + 4 steps | | | | | | | | -0.07 | 0.14 | 0.04 |
| octagon + 7 steps | | | | | | | | -0.14 | 0.20 | 0.07 |

(stops; the ring is the worst mean over rings of 30 pixels from 2.5 to 5
sigmas of the blob, on the axes or the diagonals, against the far texture;
its corners are the worst diagonal against axis, which a square-cornered ring
shows; a strip of a cell varies by about 0.03 on its own; tested to a mean of
0.06, a worst strip from 24 pixels of 0.15, a ring of 0.15 and corners of
0.06). The first cell or two are the floor's blur and the opening itself:
near a bright area a window holds less of the texture, so its minimum is a
little higher, with or without steps. The review's scene (3600 x 2400, a
0.5-stop texture beside a bright rectangle, window 14 cells), extra removal
at 24 / 48 / 96 / 160 pixels: blotchy at 3 pixels, -0.33 / -0.29 / -0.27 /
-0.04 before and -0.11 / +0.01 / -0.01 / -0.02 after; blotchy at 8 pixels,
-0.32 / -0.28 / -0.29 / -0.06 and -0.17 / -0.07 / -0.07 / 0.00; pixel noise,
-0.09 / -0.08 / -0.08 / -0.01 and -0.04 / -0.01 / -0.01 / 0.00. The texture's
log spread under Dehaze, beside the edge against far from it, went from 0.86
to 1.06 times (blotchy at 3). On the phase-7 review image (1200 x 800), the
noisy half's mean removal is -1.00 stop (-1.16 before), -1.02 in the strip
beside the bright rectangle (-1.28 before) and -1.00 further out, and the
textured lower half -1.12 (-1.18), -1.09 under the rectangle (-1.28): back to
about what the opening alone gave. The soft blob's removal is round, not
square-cornered.

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
  the veil goes from 1.54 to 2.34 at 100 (the same with the square window,
  with or without its reconstruction) and 1.37 at -100, the clear half against the veiled one from 0.63 to 0.98, and
  nothing crosses zero. On flat mid grey at 3400 pixels, Dehaze 100 takes a
  quarter-stop 128-pixel modulation (16 cells) to 2.03, noise to 1.023 and a
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
  23). Dehaze's floor is opened by an octagon of inradius 3% (23 cells: 9
  across and down, 7 a diagonal), reconstructed by two steps and blurred by a
  sigma of 0.25% (1.875 cells, radius 6); its mean, for a
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
`GpuPresenceBlock` of 320 bytes with the pass's half-width `window`, `PresenceStep`
Reduce, BlurAcross, BlurDown, MinimumAcross, MinimumDown, MaximumAcross,
MaximumDown, BlurDownAboveOpening, Reconstruct, MinimumDiagonal,
MinimumAntidiagonal, MaximumDiagonal, MaximumAntidiagonal) writes one-channel R32F targets where the device has them (ADR
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
three and positive Dehaze thirteen (the reduction; the eight passes of the
opening, `packPresence` giving the passes across and down the octagon's `a`
and the diagonal ones its `n`, all eight rendered even when `n` is zero; the
two reconstruction steps, each of which also reads the cells at binding 2 and
keeps below them; the blur across; and the last blur down, which reads the
opened grid there and keeps above it, so `presence_filter.frag` takes two
inputs, the first standing in for the second in every other step), whatever
the size: 13 renders at 24 MP where the square and its 23 steps took 30. One
fewer each when they share the reduction with Clarity: all three positive
add eighteen, and every control at zero none.

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

After the octagon (the same machine and method, 6000 x 4000 RgbU8 PNG, CPU,
release, best of five, two rounds alternating with the square and its 23
steps in one binary): Dehaze 50, context 122 / 85 ms with the square, 89 / 60
ms with the octagon, about 25 to 30 ms less; the pointwise boundary 1.28 /
0.98 s and 1.18 / 0.92 s, within the noise (no Presence 0.49 / 0.33 s; all
three 0.25 / 0.24 s context and 1.44 / 1.40 s boundary with the square, 0.21 /
0.22 s and 1.35 / 1.39 s with the octagon). On the device it is 17 fewer
renders of the 750 x 500 grid; not timed (only lavapipe is at hand).

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
window each, `a + 2n = w` cells along a row or a column, the octagon's
farthest, and each reconstruction step a cell; before the reconstruction the
formula counted the window once, which undercounted the opening): `(46 + 2 +
6 + 2) * 8` = 448 pixels at 24 MP.

**Tolerance.** The GPU's pointwise boundary with Presence agrees with the CPU
to 3.9e-6 relative on lavapipe (3.7e-6 with the octagon) (each control alone and together, both signs,
pixel scales 1 to 8, odd sizes with partial cells, a wide source with 1-pixel
and multi-pixel cells, an export-sized long edge of 3203 with 8-pixel cells
and a 23-cell opening, a bright disk whose octagonal opening the two
reconstruction steps bring back to its outline, a checkerboard of single
cells whose diagonal passes each read one parity, a source with an infinite
and a NaN pixel, the camera
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
  edge-aware filter. At a straight step it leaves no rim, and no curved edge
  of a bright area that holds the octagon does either (measured above).
- What the octagon and its two steps leave, the known limits:
  - *A convex tip whose curvature radius is below about the window:
    right-angled and acute corners and the ends of elongated bright shapes
    (lit windows, cloud streaks, reflections).* The octagon fits no tip that
    sharp, and its two steps give back only two cells, so the tip keeps the
    dark surroundings' floor: up to about 0.65 stop less Dehaze, over a
    triangle or crescent reaching in from the tip. Measured at 3600 pixels
    (cells of 8, window about 13 cells), pixels more than 0.05 stop off the
    centre reach, from the tip: 38-40 pixels for an axis-aligned rectangle,
    70-80 pixels rotated by 15 or 30 degrees (48-52 at 45), 146-163 pixels for
    a 60 degree triangle, and 40-51 pixels for the ends of an ellipse with
    semi-axes 600 and 200 (tip radius 67 pixels, 0.64 stop upright, 0.52
    turned by 30 degrees); an ellipse with a tip radius of about 125 pixels
    or more keeps none. These replace the lower figures of an earlier
    measurement (24 and 41 pixels at 2400), which summed the distances to the
    two nearest sides along the image axes and so understated rotated and
    acute tips. The square window with its 23 steps left almost none of them. More steps close the axis-aligned corner but not the rotated one,
    and bring the band back (the table above). It shows in the phase-7 image
    as a small light triangle at the rectangle's lower left corner. The
    planned edge-aware floor is the fix: it removes both the band and these
    tips.
  - *A bright area that holds no octagon of the window anywhere* (narrower
    than `2w + 1` = 47 cells, 376 pixels at 24 MP, about 6% of the long edge,
    in its widest part): its floor is its darker surroundings', and it gets
    less Dehaze than a broad area of its tone, as before. Round areas fare
    better than with the square: a disk holds the octagon from a radius of
    1.08 w rather than 1.41 w (on the review's 3600-pixel frame a 150-pixel
    disk now does, -1.322 stops to its edge, where it kept 0.13 stop more).
  - *A narrow part of a large bright area* (a tongue narrower than the
    window): brought back only two cells from where the octagon fits, where
    the square's 23 steps brought it back 23. Its floor is its surroundings'.
  - *A window of 24 cells* (long edges of 6267 to 6399 sensor pixels, and the
    like at each power of two): two steps leave at most 0.04 stop of one cell
    on a few disks just larger than the octagon.
  - *The band's remainder.* The first two or three cells beside a bright
    area take up to 0.3 stop more off a texture (the opening and the blur
    themselves, as without the steps), and a soft highlight on a texture is
    ringed by up to 0.1 stop more removal over about a window, round now
    rather than square-cornered. The far field of a texture is unchanged by
    the steps (within 0.01 stop).
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
  and off disks of 90, 150, 300 and 600 pixels under a veil up to their edges on
  the axis-facing sides and the diagonals, within 0.02 stop of the centre;
  a blotchy texture beside a bright rectangle and around a soft blob, at
  8-pixel cells, loses about as much as far from them (the bounds above); a
  bright square's right-angled corners keep less only within 48 pixels of
  their tips, and an ellipse's ends keep less only within 60 (upright) or 70
  pixels, a rounder one's none; the octagon is regular within a cell, its passes across at least
  one cell wide, and a checkerboard of cells has the dark cells' floor for
  windows of 1 to 64; the plan's two steps and the region reach;
  on flat mid grey it raises noise and a 5-pixel pattern no more than
  Texture 100, and no more than 1.2 times; an infinite pixel under negative Dehaze is not NaN; Dehaze at both signs is the same at
  every exposure; a non-finite pixel leaves its neighbours finite and the far
  pixels as they were; black stays black and the hue is kept without Dehaze; a level-2 preview within 2% of the full render's
  effect; a region is the crop of the whole render; Presence, exposure and
  white balance edits resume from Denoise bit for bit, a crop from Pointwise,
  and a Presence edit is refused at Pointwise; the curve input includes
  Presence and a curve edit still leaves it alone; the threaded context and
  chain equal one thread; non-finite values are refused; GPU parity, pass
  counts (13 for a positive Dehaze, at 60 pixels and at 3203), the pass
  half-widths in the packed block, a resume from the GPU's Denoise checkpoint
  bit for bit and the GPU tap, at production cell sizes, on a checkerboard
  and with non-finite pixels; the panel's
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
- **The opening alone, with its square window** (the first opening). It left
  the square imprint inside round bright areas described above.
- **The octagon alone.** It shrinks the crescents to at most about 0.08 of
  the window (two cells) but does not remove them: 0.21 stop at the edge of a
  600-pixel disk at 2400. Two steps of reconstruction do, which is the
  decision.
- **The square window reconstructed by as many steps as its radius** (the
  version before this one, 23 steps at 24 MP, 30 renders). It removed the
  imprint and closed right-angled corners, but its geodesic steps follow every
  8-connected path of cells at or above a level, so from a bright area they
  climbed through the brighter cells of a neighbouring texture up to a window's
  radius deep: a darker, flatter band beside every bright edge (about 0.3 stop
  over a window's width, the texture's spread down by about an eighth), a
  square-cornered ring around a soft highlight (the 3x3 step spreads as a
  square), and every textured floor lifted a little. A tolerance on the
  climb, fewer steps (11) or steps under a 3x3 erosion narrowed or shallowed
  the band without removing it (the review's grid model).
- **More steps after the octagon, to close right-angled corners.** Five at
  2400 pixels close an axis-aligned corner but not one rotated by 30°, and
  each step widens the band again (four double its mean, seven bring it to
  half the square's). An edge-aware refinement of the floor is the way to
  both; future work.
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

## Note, 2026-10-08 (the amounts)

The three amounts are `PresencePlan::amounts`, a `PresenceAmounts`
(`src/core/Presence.h`), with `presenceAmountFor` resolving one control from its
setting; `applyPresence` takes the amounts as a parameter, and the four-argument
form passes the plan's own. The shader mirrors the struct locally
(`develop.frag`). Nothing else of this ADR changes, and the output is the same
bit for bit.
