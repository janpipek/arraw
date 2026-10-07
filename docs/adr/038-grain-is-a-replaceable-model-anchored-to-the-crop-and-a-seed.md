# Grain is a replaceable model, anchored to the crop and a seed

Film grain is the second effect of the Effects pass (ADR 037). It is a
pattern in space, so it has to stay put: the same photograph shows the same
grain whether it is panned, zoomed, previewed from a pyramid level or
exported at another size, and a preview may soften it but never moves
what it can show. Grain finer than the output pixel is stood in for by a
substitute pattern, which does differ between output scales.
The algorithm will change, so it sits behind one seam; and a pattern needs a
seed, which raises who chooses it. This ADR implements the rest of D3 of the
[global adjustments plan](../ideas/global-adjustments-plan.md), with the
user's decisions there (grain as modular as possible; a hidden seed chosen by
the front end through one function).

## Decision

**Settings any model honours.** `GrainSettings` in `EffectsSettings`
(`DevelopSettings::effects.grain`):

| Key | Range | Default | Meaning |
|---|---|---|---|
| `grainAmount` | 0 to 100 | 0 | strength; 0 is off |
| `grainSize` | 0 to 100 | 50 | size of a grain relative to the cropped frame's long edge |
| `grainRoughness` | 0 to 100 | 50 | 0 is even, one scale; higher mixes in coarser clusters |
| `grainModel` | `valueNoise` | `valueNoise` | the algorithm, an enumeration |
| `grainSeed` | 0 to 2^32 - 1 | 0 | which of the model's patterns; 0 is "none chosen" |

Group `Effects`, every photograph, `Stage::Effects`. Amount, size and
roughness are `main`'s controls and ranges. A new algorithm is a new
`GrainModel` value with a stable name in `grainModelNames`, never new
settings, and a document keeps the model it was made with. The codec spells
the model by name and the seed as an exact whole number: a seed is an
identity, so one out of range or with a fraction is malformed rather than
clamped or rounded into another pattern.

**The seed is the photograph's own.** `FieldDescriptor` gains a
`SettingScope`, `Look` by default; `grainSeed` alone is `Photo`. No panel
row shows a `Photo` setting (the test of what the panel shows counts it as
neither shown nor pending), and future presets and copy/paste leave it on
the photograph they are applied to, as `main` did. It lives in the settings,
and so in the descriptor table, sidecar, JSON, CLI and Python for free,
rather than next to them in `DevelopState` as ADR 021 foresaw: it is one
number with a fixed row, not a list, and the scope gives it what ADR 021
wanted, staying with its photograph.

**The core never invents randomness.** `grainSeed = 0` renders with one fixed
constant (`unseededGrainSeed`), so a document without a seed is still
deterministic and a plan stays a pure function of the state.
`arraw::chooseGrainSeed(previous, next, entropy)` is the single policy
point, and its rule is one transition: a seed is chosen only when an edit
turns grain on, its amount going from 0 (or anything the plan clamps to 0)
to above 0, and `next` has seed 0. It then gets 32 bits from `entropy` (by
default `std::random_device`), never 0. Every other edit keeps `next`'s seed,
0 included, so grain that was already on keeps its pattern, even the fixed
one of seed 0. Front ends call it with the settings before and after an edit
and store the result:
- the develop panel after every slider edit, with the state it showed as
  `previous`: grain turned on gets a seed once and keeps it through later
  edits and through being turned off; a photograph whose grain is on with
  seed 0 (a hand-written sidecar, one from the command line or an older
  build) keeps the fixed pattern through unrelated edits;
- the command line per photograph after applying the flags, with the
  sidecar's (or the bare photograph's) settings as `previous`: grain the
  flags turn on gets a random seed per export, since the command never
  writes it back; grain the sidecar already has keeps its seed, 0 included,
  so its exports repeat and match the GUI. An explicit `--grain-seed`,
  including 0, is stored as given and the policy is not asked
  (`--grain-model` names the model);
- Python exposes it as `arraw.choose_grain_seed(previous, next,
  entropy=None)`; `develop` itself does not call it.

Deriving the seed from the photograph instead (plan option b) would change
only `chooseGrainSeed`.

**One seam between the pass and the model.** `GrainPlan` (`GrainModels.h`)
is what the settings resolve to, the same for every model: `active`, the
model, the deviation, the size as a fraction of the long edge, the roughness
from 0 to 1 and the seed (never 0). An amount of 0 is the default plan
whatever the rest says, so the plan, the pixels and the GPU's passes are as
without grain. Per render, `grainPlacementOf(plan, mapping)` asks the model to
place itself on the output pixels (`GrainPlacement`), and per pixel
`grainAt(plan, placement, column, row)` asks it for the grain. Both switch on
the model and call one function each (`valueNoisePlacement`,
`valueNoiseGrain`); `effectsPixel` knows neither. The shader mirrors it:
`grainAt` switches on the block's `grainModel` and calls `valueNoiseGrain`.
Each model owns its band-limit. A model that needs other per-render data
widens `GrainPlacement`; one that needs other settings is not a grain model.

**Grain is added in the perceptual coordinate, monochrome.** `applyGrain` maps
each channel to the signed `v = y^(1/2.2)` (ADR 010), adds the same grain to
all three and maps back, so the texture's contrast looks alike in shadows and
highlights, as `main` added it in sRGB's encoding. It runs after the
vignette, on the vignetted tones. A grain of exactly 0 returns the colour bit
for bit.

**The first model is `main`'s value noise.** Up to three lattices at 1,
1/0.53 and 1/0.23 grains a cell, each with its own seed (`main`'s salts).
Each holds a value per cell from `grainHash(x, y, seed)` (`main`'s integer
hash, modulo 2^32 on both backends), centred to (-0.5, 0.5) symmetrically,
and interpolates with smoothstep weights. Roughness mixes from the finest
lattice alone to `0.6 : 0.3 : 0.1` of the three; unlike `main`, the mix is
normalised, so the field's deviation is the plan's at every roughness. The
lattices are independent, and smoothstep interpolation keeps `(26/35)^2` of a
lattice's variance, so the unit scale is `sqrt(12) * 35 / 26` (`main` used
4.9). The deviation is `0.08 * amount / 100` in the perceptual coordinate
(`main`'s 0.08). Size spreads logarithmically from `main`'s 0.5 to 4 pixels of
a 2048-pixel long edge.

**Positions are placed in double on the host, small on the device.** Each
lattice is placed on a render as an integer cell for output pixel (0, 0), the
float fraction of that cell, and the float cells per output pixel, all
worked out from the crop-frame mapping (ADR 037) in double. Pixel `(x, y)`
is at `cell + (fraction + (x, y) * delta)`: the float part spans only the
cells one render covers, so it is as exact at a 16x zoom into a large frame
as at fit-to-screen, which crop-frame floats alone would not be. The CPU and
the shader evaluate the same correctly rounded operations in the same order
(the shader marks them `precise`, the CPU compiles `GrainModels.cpp` with
`-ffp-contract=off`), so they find the same cells and lattice values; only
`applyGrain`'s powers differ. A region renders the grain of the whole frame
at the same place, up to the float rounding of the fraction.

**Band-limited by pixel footprint, with a pixel-scale substitute.** A
lattice is faded by a smoothstep in the size of its cells in output pixels:
left out at one pixel or less, whole at two or more. Under a pixel the
lattice is sampled more sparsely than its values are spaced and would alias
into another pattern; between one and two pixels every pixel samples its
cell at nearly the same phase, and the grain's contrast would beat with the
pixel grid (measured: at exactly one pixel a cell, two thirds of the
deviation).

What the fade takes away is not thrown away. A larger render downscaled
keeps part of fine grain as finer, weaker noise, so the model works out per
lattice the share of its variance a box filter of the output pixel would
keep (the integral of the square of the smoothstep hat convolved with the
box, per axis: `26/35` for no box, about `1 / width` for a wide one), takes
away what the faded lattice still draws, and puts the rest into a fourth
lattice, the substitute. Its cells are `grainSubstituteCell` (two) output
pixels on the crop frame: the cell of a frame point depends only on the
render's step, so it does not swim as the render pans, but it is another
pattern at every output scale, which is acceptable because it stands in only
for detail no render at that scale can show. At two pixels a cell every
pixel samples one of two phases half a cell apart, so it cannot alias, and
its weight is set from those two phases so that it adds exactly the missing
variance. It has its own salt, and the GPU draws it as the fourth layer with
the same arithmetic as the other three.

A smaller render, such as a preview of a pyramid level, therefore samples
the same coarse lattices at the same frame points and only replaces what it
cannot show: softer, never rearranged where the grain is visible, and as
grainy, statistically, as the full render downscaled. Coarser lattices are
kept whole even where a box would soften them slightly, so a preview may be
a little grainier than the downscale (up to about 14% in RMS where a lattice
sits just above two pixels a cell).

**The GPU block.** `GpuEffectsBlock` is 176 bytes: ADR 037's 40, then
`grains`, `grainModel` and four std140 `GrainLayer` structs (cell, fraction,
delta, weight, seed; 32 bytes each), packed by `packEffects` from the same
`grainPlacementOf` the CPU uses.

## Consequences

- A grain edit, a reseed included, is an effects edit: it resumes from the
  resize checkpoint (one cheap pass), and the effects checkpoint refuses
  another seed.
- Parity: the effects pass with grain agrees with the CPU to 1.6e-6 relative
  on lavapipe (five settings after an 8x enlargement, with and after both
  vignettes; a region, a shrink, and a 16x zoom into a 256-pixel frame), held
  to the pointwise tolerance plus the resample tolerance, as the vignette is.
- Tests hold: zero mean (within 3% of the deviation) and the plan's
  deviation (within 6%) at roughness 0, 50 and 100, on the model and on a
  developed grey; the amount scaling it linearly; every render of a state
  identical; seeds uncorrelated and seed 0 the fixed pattern; a region at 1:1,
  at 4x and two overlapping regions at 16x matching to 1e-5; the fade at one,
  one and a half and two pixels a cell, with the substitute taking over and
  the deviation rising with the render's size; grain at the default size on
  an 1800-pixel render keeping 0.85 of the deviation (roughness 0 and 50,
  band 0.75 to 0.95), and the finest size at roughness 0 on a 1600-pixel one
  keeping 0.44 (band 0.4 to 0.7); renders 4 and 8 times smaller matching the
  RMS of a larger render boxed down within 15% (measured within 6%, sizes 0
  and 40, roughness 0, 50 and 100); a preview from a pyramid level
  correlating with the full render boxed down (0.33 with the substitute,
  0.59 for its lattices alone, against 0.00 for another seed) with an RMS
  1.14 times the boxed one, the reduced placement on the same lattice
  positions, and the substitute staying put under a pan; amount 0
  bit-identical with no pass, whatever the rest says; the panel choosing a
  seed only when grain is turned on, and keeping seed 0 for grain already
  on; the command line's seed and model flags, and at export level
  `--grain-seed 0` and a sidecar with grain and seed 0 repeating the fixed
  pattern, and grain the flags turn on being random per export.
- Grain finer than two output pixels is not drawn as itself but as the
  substitute, whose pattern changes with the output scale (a zoom step, an
  export at another size) while its strength follows what a downscale would
  keep. The grain set never vanishes; a smaller render shows less of it
  only as a downscale would. Measured on the command line (1600 by 1066,
  amount 100, seed 7, RMS over a flat patch in 8-bit units, against about
  20 intended): roughness 0 gives 8.9, 15.3 and 20.2 at sizes 0, 40 and
  100; roughness 50 gives 9.3, 15.6 and 20.0. Before the substitute these
  were 0.0, 0.0, 20.3 and 1.2, 3.9, 20.0.
- The substitute matches a downscale of grain drawn whole, not of grain
  that is itself a substitute: its two-pixel cells hold more coarse variance
  than real sub-pixel grain, so a render that already uses it keeps more
  when downscaled again. A 400-pixel preview of the 1600-pixel export above
  has 0.56 to 0.73 of the RMS of that export boxed down to 400 pixels where
  the export uses the substitute (sizes 0 and 40), and 0.98 to 1.01 where
  it does not (size 100).
- Grain the command line's flags turn on is random per export unless
  `--grain-seed` is given, because it never writes the seed it chose back;
  grain a sidecar already has is as the GUI renders it.
- Copy/paste and presets do not exist yet; when they do, they skip
  `SettingScope::Photo` rows.

## What was rejected, and why

- **Grain in crop-frame floats on the GPU.** A frame position is good to about
  1e-7 of the frame; at 16x into a large frame, with thousands of cells an
  edge, the shader would find other cells than the CPU near their edges.
- **No band-limit, as `main`.** A preview point-samples grain finer than its
  pixels into a different, aliased pattern, which the next zoom rearranges.
- **Fading from half a pixel to one.** Keeps more grain on screen, but lets an
  aliased lattice through at part weight and beats with the pixel grid where
  cells are about a pixel.
- **Dropping what the fade leaves out** (this ADR's first version). Grain
  the photographer turned on vanished at ordinary sizes: amount 100,
  roughness 0, size 40 on a 1600-pixel export was bit-identical to no grain,
  and a 400-pixel preview showed none.
- **A substitute anchored to the output pixels.** Simpler, but it would swim
  as the render pans.
- **Re-basing the size scale** so the finest grain is two pixels of a modern
  long edge. Exports would keep their grain, but the sizes would no longer
  mean what they do in `main`, and previews would still lose it.
- **A random seed chosen by the core when grain turns on.** The plan would no
  longer be a function of the state; the seed would differ between a preview
  and its export.
- **The seed next to the settings in `DevelopState`** (ADR 021's sketch). It
  would need its own codec, sidecar field and front-end plumbing for one
  number; a scope on its row gives the same "stays with the photograph".
