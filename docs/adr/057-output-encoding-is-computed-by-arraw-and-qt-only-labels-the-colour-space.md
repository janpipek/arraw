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
   (1.4054, 0.6484, 0.4185)); the export sharpening works on that float result and quantises after.
   Alpha is straight, never premultiplied, and only rounded to the target depth. Layouts without
   alpha drop it; sources without alpha are opaque. Camera-native buffers are still refused.
6. **`toDisplayImage` delegates** to `encodedForOutput`, so the window, the exports and the
   histogram are one implementation, which the byte-identity test of ADR 055 now holds trivially.
7. **Rows are split over threads** with `detail::splitRowBands`, with no progress counted, as the
   sharpening does; any split gives the bits of the single-threaded loop.

## Not done

- **Import** (`ImageImport.cpp`) still converts with Qt. Its quantisation sits before the
  development, not at the end of it; whether it matters is to be measured before it is replaced.
- **No lookup tables.** The exact `pow` is affordable (see the measurements in the change that
  introduced this ADR); a table is how the bug arose.
