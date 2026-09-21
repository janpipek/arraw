# Test fixtures

Binary inputs for the test suite, together with the scripts that produce them.

Two generators, one per input kind:

| Script | Produces | Read by |
|---|---|---|
| `make_fixtures.py` | `*.png` — a 61×41 test card | `test_RoundTrip.cpp` |
| `make_raw_fixtures.py` | `*.dng` — synthetic 32×24 RAWs | `test_RawImport.cpp` |

The **generated files are committed and are what the tests read**. The scripts
are committed so that a reader can see what is in them and why, and so that new
variants are cheap to add — neither is run by the build.

> Change a generator → run `just fixtures` → commit the regenerated files.
>
> Nothing enforces this. The tests read the committed files, so a forgotten
> regeneration does not fail anything; it just leaves a script describing an
> image that is no longer on disk.

## Why a generator rather than images written by Qt

The round-trip test asserts that `loadImage` followed by `exportImage`
preserves an image. If Qt had also written the input, a Qt codec bug would
cancel itself out and the test would pass straight through it. The generator
uses `zlib` and `struct` only — no image library, least of all the one under
test, has a hand in the ground truth. See ADR 004.

## The test card

All fixtures render the same 61×41 card. The dimensions are odd on both axes so
a row-stride bug cannot hide behind four-byte alignment.

| Rows  | Band                                        |
|-------|---------------------------------------------|
| 0–7   | Saturated hue sweep, left to right          |
| 8–13  | Grey ramp, 0 → 255                          |
| 14–19 | Red ramp                                    |
| 20–25 | Green ramp                                  |
| 26–31 | Blue ramp                                   |
| 32–40 | Twelve flat patches, 5 px wide (last is 6)  |

The bands differ in height and content, and every ramp runs dark-to-bright left
to right, so a vertical or horizontal flip is obvious by eye.

The patches, left to right, are `black(0)`, `code1(1)`, `code2(2)`, `code3(3)`,
`quarterGrey(64)`, `midGrey(128)`, `threeQuarterGrey(192)`, `nearWhite(252)`,
`white(255)`, `red`, `green`, `blue`. The first four exist because a linear
working space is at its coarsest in deep shadow, and the near-white ones because
that is where a transfer function that overshoots would clip.
`test_RoundTrip.cpp` asserts these positions by name, so a failure reads
"midGrey: expected 128, got 121" rather than a pixel count.

## The fixtures

| File | Depth | Channels | Colour tagging | Used by |
|---|---|---|---|---|
| `testcard-61x41-srgb8.png` | 8 | RGB | `sRGB` chunk | `test_RoundTrip.cpp` |
| `testcard-61x41-srgb16.png` | 16 | RGB | `sRGB` chunk | *not yet* |
| `testcard-61x41-grey8.png` | 8 | Grey | `sRGB` chunk | *not yet* |
| `testcard-61x41-alpha8.png` | 8 | RGBA | `sRGB` chunk | *not yet* |
| `testcard-61x41-untagged8.png` | 8 | RGB | none | *not yet* |
| `testcard-61x41-adobergb8.png` | 8 | RGB | `iCCP`, Adobe RGB (1998) | *not yet* |

### Please do not delete the unused ones

`loadImage` has **no unit tests**. The round-trip test exercises exactly one
path through it: 8-bit, RGB, sRGB-tagged. The five other fixtures are the inputs
for the import tests that should follow, and each targets a specific untested
branch:

- **srgb16** — must not be narrowed to eight bits on the way in.
- **grey8** — a single-channel source must arrive as neutral RGB, not a channel
  swap or a broadcast into one channel.
- **alpha8** — straight alpha ramps across the full width, including fully
  transparent and fully opaque columns. `convertedToColorSpace` on a 16-bit
  RGBA layout is where Qt could silently premultiply; a round trip through
  premultiplication is lossy near alpha 0, which is why this fixture is kept out
  of the "close to the original" test. It is also the natural input for
  `exportImage`'s refusal to write a transparent JPEG.
- **untagged8** — exercises the "an untagged file is sRGB" fallback in
  `src/core/ImageImport.cpp`. It must decode identically to `srgb8`.
- **adobergb8** — byte-identical pixels to `srgb8` but a different meaning, so
  honouring the embedded profile is observable: it must decode to *different*
  colours. The profile is a minimal matrix/TRC v2 profile built by the script,
  because no dependency-free portable way to obtain a real one exists.

## The RAW fixtures

Synthetic DNGs — a DNG is a TIFF with extra tags, so `make_raw_fixtures.py`
writes them with `struct` alone. All are 32×24, uncompressed, 16-bit, and
declare camera space as linear sRGB via `ColorMatrix1`, so a neutral input stays
neutral and any colour cast in a decode is a bug rather than an artefact.

Adapted from `make_test_dng.py` on the `main` branch, which produced only the
first of these.

| File | Layout | As-shot neutral | Orientation | What it can observe |
|---|---|---|---|---|
| `linear-32x24-neutral.dng` | LinearRaw, 3 spp | unity | none | `no_auto_bright`, linear output gamma |
| `linear-32x24-warmwb.dng` | LinearRaw, 3 spp | (0.5, 1.0, 0.8) | none | `use_camera_wb` |
| `linear-32x24-rotated.dng` | LinearRaw, 3 spp | unity | 6 (90° CW) | `user_flip = 0`, and *which decoder ran* |
| `bayer-32x24.dng` | CFA RGGB, 1 spp | unity | none | demosaic actually runs |
| `linear-32x24-highmax.dng` | LinearRaw, 3 spp | unity | none | `adjust_maximum_thr = 0` |
| `linear-32x24-nowb.dng` | LinearRaw, 3 spp | **absent** | none | the missing-white-balance fallback |
| `linear-32x24-nowb-dark.dng` | LinearRaw, 3 spp | **absent** | none | that the fallback ignores the frame |
| `preview-32x24.dng` | RGB preview + LinearRaw sub-IFD | unity | none | *which image* was decoded |

All but the first exist because the first cannot see what it does not contain.
A LinearRaw file with unity white balance and no orientation decodes
identically whether the decoder demosaics or not, honours the as-shot neutral
or not, and rotates or not.

The last four came out of the review in `docs/reviews`, and each one pins a
decode setting that was wrong while no fixture could tell:

- **highmax** is two flat halves, 16000 and 52000, under a declared white level
  of 65535. 52000 sits above LibRaw's `adjust_maximum_thr` of 0.75, so LibRaw
  would take the frame's own maximum for the white level and stretch 16000 to
  20164. The ramp fixtures reach 65535 and so have nothing to lower.
- **nowb** and **nowb-dark** are the same flat (48000, 32000, 16000) field with
  no `AsShotNeutral` tag, differing only in a darkened right half. A white
  balance computed from the frame flattens the colour to grey *and* differs
  between the two; the daylight fallback does neither. One file could only
  observe the first half of that.
- **preview** has the shape of a real camera file — an ordinary 8×6 RGB preview
  in IFD0, the sensor data in a sub-IFD — and is the only fixture whose two
  possible answers are both valid images. Every other RAW fixture here is
  single-IFD, which no camera writes, and a single-IFD file cannot show a
  decoder returning the preview instead of the photograph. Its sub-IFD repeats
  the neutral ramp, so the photograph is identifiable pixel for pixel, and the
  preview is flat magenta, which the ramp never is.

**`linear-32x24-neutral.dng` carries the sharpest assertion in the suite.** It
is a neutral linear ramp, so a correct decode has nothing to do but a
colour-space rotation: measured max error is **1 code in 65535**. An automatic
brightness stretch would miss by thousands, a stray transfer function by tens of
thousands.

**`linear-32x24-rotated.dng` doubles as a decoder discriminator.** Where KDE's
`kf6-kimageformats` is installed, its LibRaw-backed `kimg_raw.so` plugin
registers with Qt and honours the orientation tag, returning 24×32; arraw must
return 32×24. That is the only way to assert from outside `loadImage` that the
plugin did not quietly win, and it matters because the plugin is present on some
developer machines and absent in CI.

The plugin claims files by **extension**, not signature — a renamed DNG is
detected as `tiff` — so the discriminator only bites for an extension the plugin
claims and `loadImage` does not route to LibRaw by name. `test_RawImport.cpp`
copies this fixture to `holiday.mrw` for exactly that reason; `.mrw` is one of
eleven such extensions (`.mrw .srf .x3f .kdc .mos .raw .3fr .iiq .erf .nrw
.crw`). A copy named `holiday.png` exercises the content check instead, and
cannot tell the two decoders apart — which is what `preview-32x24.dng` is for:
copied to those same names, it tells whether the content check ran *before* Qt
or only after Qt failed. See ADR 005.
