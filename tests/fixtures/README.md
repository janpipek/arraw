# Test fixtures

Binary inputs for the test suite, together with the script that produces them.

The **PNGs are committed and are what the tests read**. `make_fixtures.py` is
committed so that a reader can see what is in them and why, and so that new
variants are cheap to add — it is not run by the build.

> Change `make_fixtures.py` → run `just fixtures` → commit the regenerated PNGs.
>
> Nothing enforces this. The tests read the committed files, so a forgotten
> regeneration does not fail anything; it just leaves the script describing an
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
