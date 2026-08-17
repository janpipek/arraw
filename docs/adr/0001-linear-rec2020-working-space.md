# Linear Rec.2020 as the working color space

The pipeline originally worked in linear sRGB primaries (libraw default), which makes
wide-gamut export meaningless — the gamut is clipped at decode. We chose linear
Rec.2020 (`output_color=8`, requires libraw ≥ 0.21) over ProPhoto (Lightroom's choice)
because ProPhoto's two imaginary primaries let saturation/HSL math produce physically
meaningless pixel values that surface as artifacts only after the output transform;
Rec.2020 has all-real primaries and fully contains both target output gamuts
(Display P3, AdobeRGB).

**Camera input profiles (DCP/ICC) are explicitly out of scope.** arraw takes
libraw's colorimetric decode as the input transform and does not apply a
per-camera look profile. Adding one later would change the rendering of every
existing sidecar, so it is a decision deferred deliberately, not an oversight.

## Consequences

- Luma weights in `image.frag` change from Rec.709 (0.2126, 0.7152, 0.0722) to
  Rec.2020 (0.2627, 0.6780, 0.0593).
- Existing XMP sidecars were authored against sRGB-primaries rendering; the same
  slider values now render slightly differently. Accepted — no compatibility flag.
