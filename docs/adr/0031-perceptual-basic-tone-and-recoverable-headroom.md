# Basic Tone is perceptual, endpoint-aware, and keeps recoverable headroom

The old Basic Tone implementation added four masked luminance offsets after a
scene-linear Exposure multiply and an affine Contrast operation. A positive
Shadows value therefore added almost the same amount to every very dark nonzero
pixel: exact black happened to remain black because of an RGB scaling special
case, while an arbitrarily small value jumped toward grey. Negative values made
flat crushed regions. Exposure clipped highlights quickly, and Contrast moved
both endpoints. The controls did not have distinct photographic jobs.

arraw now treats the six Basic Tone controls as one perceptual luminance mapping:

- **Exposure** moves a wide midtone range with stop-like calibration.
- **Contrast** expands or contracts perceptual midtones.
- **Shadows** and **Highlights** reshape their regions while anchoring black and
  white respectively.
- **Blacks** and **Whites** deliberately control the clipping points. They are
  the only Basic Tone controls intended to move the endpoints.

This follows Lightroom's documented division of responsibilities, but it is not
an attempt to clone Adobe's proprietary, image-adaptive Process Version 2012
algorithm. The goal is predictable photographic behavior with equations we own
and can test.

## The mapping

Let `y` be linear Rec.2020 luminance and let `x = max(y, 0)^(1/2.2)` be its
perceptually encoded coordinate. The controls run in this order.

Exposure is a translation in log-odds space:

```
k = 2^Exposure
x = kx / (1 - x + kx)
```

This is applied for `0 < x < 1`. It anchors 0 and 1, positive and negative values
are exact inverses, and `+1 EV` maps 18% linear grey to approximately 36% while
compressing smoothly toward white instead of clipping it. The `EV` label means
perceptual, stop-like exposure; it is not a literal multiply of every working
value.

Contrast is a scaling in the same log-odds space:

```
a = 2^(Contrast / 100)
x = x^a / (x^a + (1 - x)^a)
```

It fixes encoded middle grey (`x = 0.5`, about `y = 0.218`) as well as both
endpoints. Positive Contrast separates the two sides; negative Contrast brings
them together. Opposite settings are exact inverses.

Shadows and Highlights use soft endpoint anchors. With slider values `s` and
`h` in `[-100, 100]`:

```
shadowMask    = 1 - smoothstep(0.35, 0.65, x)
highlightMask =     smoothstep(0.35, 0.65, x)

x += 0.1 (s / 100) [x / (x + 0.1)] shadowMask
x += 0.1 (h / 100) [(1 - x) / (1 - x + 0.1)] highlightMask
```

The bracketed factors make the regional change vanish continuously at its
endpoint. Positive Shadows increases the slope out of black rather than painting
a fixed grey floor; negative Shadows can compress that slope to zero at `-100`
without reversing tones. Highlights is the mirrored behavior at white.

Blacks and Whites are intentionally different. They move only the ends of the
encoded range, using narrower masks:

```
x += 0.1 (Blacks / 100) [1 - smoothstep(0.1, 0.4, x)]
x += 0.1 (Whites / 100) [    smoothstep(0.6, 0.9, x)]
```

Negative Blacks creates black clipping; positive Blacks can lift source black to
a neutral grey because hue is undefined at zero. Positive Whites creates
intentional highlight clipping for high-key/specular looks; negative Whites can
bring values above 1 back into display range. The result is decoded with
`y' = max(x, 0)^2.2` and normally scales RGB by `y'/y` to preserve hue.

## One tested model for global and masked tone

`BasicTone` is a deep CPU module: it owns the equations and builds a 256×17 float
LUT atlas. Row 0 is the global mapping; rows 1–16 contain the same mapping for
each Local Adjustment. The shader samples those rows, so CPU tests exercise the
model that supplies the real renderer rather than a separately reimplemented
approximation.

A Local Adjustment blends its fully mapped result with the incoming pixel using
the Mask weight. This makes a 50% Mask a 50% visual blend and lets later Local
Adjustments operate on the result of earlier ones.

## Values above white stay live

Working values above 1 are **Recoverable Headroom**, not invalid pixels. The LUT
stores its upper slope so the shader can extrapolate beyond 1. The user Tone
Curve likewise extrapolates from its final two LUT samples instead of clamping at
its last sample. White balance, HSL, saturation, Local Adjustments, vignette, and
grain therefore receive the live value; for example, a later masked negative
Whites adjustment can turn `1.03` into a value below 1.

Clipping happens only at an actual output boundary:

- The display transform clamps to the monitor range; the Clipping Overlay judges
  the pre-clamp value.
- Export rendering reads back an unbounded float working-space image.
- The CPU output transform receives that float image, and bounded 8/16-bit output
  formats clip when encoded. A future HDR output can retain the same headroom
  without changing the develop pipeline.

## Consequences

Existing sidecars retain the same Lightroom-compatible field names and slider
values, but they render differently because their interpretation has changed.
The new behavior is deliberately not bit-compatible with the old bug. The LUT
adds one small texture upload when tone parameters change; ordinary repaint,
zoom, and pan reuse it. GPU integration tests remain conditional on a working
RHI context, while the complete scalar model and LUT construction run headlessly.
