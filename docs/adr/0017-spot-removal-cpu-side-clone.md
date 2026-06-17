# Spot removal is a CPU-side clone applied to the decoded buffer, not a shader operation

Spot removal (sensor dust, minor distractions) operates by copying a source circle of
pixels onto a destination circle in the decoded `ImageBuffer`, with a feathered blend at
the boundary — before the buffer is uploaded to the GPU. Every other per-pixel adjustment
lives in the fragment shader, so this is the deliberate exception.

## Considered Options

- **Shader-side clone (texture self-sampling).** A spot uniform would carry a source UV
  offset; the fragment shader would sample the texture at `uv + offset` and blend it into
  the destination circle. Keeps everything single-pass and GPU-resident. Rejected because
  it would require extending the `Ubuf` uniform block, the shader, and the 16-mask cap
  discipline from ADR 0010 — complexity that buys nothing: clone is cheap CPU pixel math.
- **Heal (Poisson blending).** Adapts the pasted content to the colour/tone of the
  surrounding destination region. Rejected for v1: requires an iterative multi-pass solve,
  breaking the single-pass invariant. Sensor dust on smooth backgrounds (the primary use
  case) is indistinguishable from true heal when a nearby source region is chosen.

## Consequences

- `MainWindow` holds the clean decoded buffers (`fullRes`, `preview`) and a cached
  `spottedFullRes` derived by applying all spots. The clean buffers are never mutated.
  Export and the full-res texture upload use `spottedFullRes`; preview is re-spotted on
  every spot gesture, full-res on mouse-release and undo/redo.
- `Spot` is a distinct domain concept from `LocalAdjustment`: no tonal parameters, no
  parametric mask, coordinates in original buffer pixels (not display-frame UV). It is not
  a shader uniform.
- Spot coordinates are stored in absolute buffer pixel coordinates in the arraw XMP
  namespace — resolution-coupled, but spots are per-image and never transfer between
  images, so normalisation buys nothing.
- A `displayUVToBufferPixel` utility (in `arraw_core`) converts viewport cursor positions
  to buffer pixel coordinates for the spot-placement UI, inverting the crop + rotation
  transform that the vertex shader applies.
