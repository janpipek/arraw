# Output encoding is computed by arraw, and Qt only labels the colour space

Status: accepted, 2026-11-09; implemented.

`encodedForOutput` (`src/core/EncodedForOutput.cpp`) turns a developed buffer into the pixels an
export writes, the display shows and the output histogram counts
([ADR 055](055-the-histogram-panel-counts-the-picture-as-shown-on-a-thread-of-its-own.md)). It
used `QImage::convertedToColorSpace`. This ADR replaces that with arithmetic of our own.

## Why

Qt's colour transform runs through a coarse lookup table, so a 16-bit export is effectively 11 to
12 bits. Measured on linear Rec.2020 float to 16-bit sRGB, for a ramp rising 1% from a start value:

| Ramp starts at | Codes Qt produced | Codes the exact curve gives |
|---|---|---|
| 0.001 | 1 | 9 |
| 0.05 | 3, in steps of 41 | 83 |
| 0.36 | 15, in steps of 13 | 188 |

The same quantisation made the GPU-against-CPU export parity test fail: two floats two ulps apart
(0.365808845 and 0.365808785, linear Rec.2020) landed 25 codes apart after Qt's conversion. Banding
in a smooth sky is what a photographer would see; the tolerance in that test hid it.

## Decision

1. **One conversion, written once.** Per pixel: the source's transfer curve to linear (exact
   formula), a 3x3 change of primaries, a clamp to [0, 1] for integer layouts, the target's exact
   curve, and one rounding to 8 or 16 bits (or none, for float). All arithmetic is in double.
2. **Curves live in `src/core/TransferFunctions.h`**, the only place they are written (the output
   histogram's tables use them too). sRGB and Display P3 share the sRGB piecewise curve, Adobe RGB
   is a pure gamma of 563/256, and the perceptual coordinate a pure 2.2. All are odd below zero.
   The luma weights of the output histogram are row Y of the encoding's derived matrix, and the
   hue picker encodes with `encodeSrgb`. The float perceptual curve of the develop code
   (`toPerceptual`, `toLinear`) is the same curve in float for speed and takes its exponent from
   `transfer::perceptualGamma`; a test holds the two precisions together. Constants that must also
   exist in GLSL stay as copies, as before.
3. **Matrices come from chromaticities, in `src/core/ColorSpaces.h`**: each space's RGB-to-XYZ is
   derived from its primaries and the shared D65 white, and the source-to-target matrix is composed
   from them. All spaces are D65, so there is no chromatic adaptation. Where two encodings share
   primaries (sRGB to sRGB, linear to perceptual Rec.2020) the matrix is skipped, so a round trip
   through a curve alone stays exact. The existing float `srgbToWorking` is unchanged; a test pins
   it to the derived matrix.
4. **Qt only labels.** The result carries the target's `QColorSpace` so that profiles are embedded;
   no pixel value comes from Qt.
5. **Behaviour kept from Qt.** Integer layouts clamp each channel to [0, 1]. `RGBA32FPx4` does not
   clamp, and extends the curves odd below zero (linear Rec.2020 (1.5, 0.5, 0.2) gives sRGB
   (1.4054, 0.6484, 0.4185)); the export sharpening works on that float result and quantises after, through
   `quantisedForOutput`, which is the same clamp and rounding as the unsharpened path.
   Alpha is straight, never premultiplied, and only rounded to the target depth. Layouts without
   alpha drop it; sources without alpha are opaque. Camera-native buffers are still refused.
6. **`toDisplayImage` delegates** to `encodedForOutput`, so the window, the exports and the
   histogram are one implementation, which the byte-identity test of ADR 055 now holds trivially.
7. **Rows are split over threads** with `detail::splitRowBands`, with no progress counted, as the
   sharpening does; any split gives the bits of the single-threaded loop.

## Not done

- **Import** (`ImageImport.cpp`) still converts with Qt. Its quantisation sits before the
  development, not at the end of it; whether it matters is to be measured before it is replaced.
- **No lookup tables.** The exact `pow` is affordable (measurements below); a table is how the bug
  arose.

## Consequences

- **Cost of the display conversion.** Measured in a Release build, best of five, 8 cores,
  RgbaF32 working encoding to RGBA8888 sRGB (`[display-bench]` in `tests/test_EncodedForOutput.cpp`):

  | Size | Qt `convertedToColorSpace` | `encodedForOutput` |
  |---|---|---|
  | 1920x1080 | 59.5 ms | 35.0 ms |
  | 2560x1440 | 117.6 ms | 63.8 ms |
  | 3840x2160 | 269.2 ms | 150.8 ms |
  | 5120x2880 | 481.6 ms | 291.0 ms |

  The exact conversion is not slower than Qt's at any of these sizes, but about 1.8x faster, so the
  preview did not get dearer; an earlier worry that it doubled came from a 16-bit export figure,
  not from the display. Still, 150 ms for a 4K frame is a lot; if a profile ever shows it on the
  slider path, an exact 8-bit fast path (the 255 decision boundaries of the encode, found by
  comparing the linear value, which gives the same bits as `pow` without calling it) is the lever.
  It was not built, as it would be a second implementation of the rounding for no gain today.
- **Threads.** Each call splits its rows into up to one band per hardware thread, started as
  `std::jthread`s for that call (the caller takes the first). A 4K frame is 8 bands, 7 thread
  starts, negligible against the 135–150 ms the frame takes. Concurrent callers (preview, histogram, thumbnails) do
  oversubscribe the cores: 3840x2160 took 135 ms alone in that run, 318 ms for two at once and 606 ms for four,
  so the loss is about 15% on top of sharing, with no cliff. A shared pool would remove it, and is
  a change to `splitRowBands` for every caller, not to this one.
- **One rounding.** A sharpened export and an unsharpened one, a display and an 8-bit export, round
  with the same code. A display and an unsharpened 8-bit sRGB export are byte-identical (tests hold
  this). A sharpened export stores the encoded values as 32-bit floats before it rounds, so even
  where sharpening changes nothing about 0.085% of 16-bit samples land one code from the
  unsharpened file (about 3e-6 at 8 bits).
- **Luma weights moved slightly.** The histogram's weights are now the exact ones of the primaries
  (sRGB Y of red 0.212639, from the BT.709 chromaticities and D65 at (0.3127, 0.3290), where the
  widely copied matrix built on a slightly different D65 has 0.2126729; both round to BT.709's
  published 0.2126), shifting a few
  pixels per image across a luma code.

## What was rejected, and why

- **Keeping Qt's conversion.** It is the bug: a lookup table coarse enough to cost four to five
  bits of a 16-bit export, with no setting to refine it.
- **A coarser or interpolated lookup table of our own.** Fast, but approximate in exactly the dark
  and steep parts where banding shows, and a second thing to keep in step with the formulas. The
  exact conversion turned out cheaper than Qt's, so a table buys nothing.
- **lcms2 or another colour management library.** A dependency for what is three curves and a 3x3
  matrix, and it would bring its own quantisation choices and a build step on every platform.
- **`QColorTransform` at higher precision.** Qt offers no such control; the table is internal.
- **Silencing the parity tests with a tolerance.** Two tests had been loosened for Qt's rounding;
  they now compare exactly (the CLI grain test) or check the 16-bit picture (the GPU lens test).
