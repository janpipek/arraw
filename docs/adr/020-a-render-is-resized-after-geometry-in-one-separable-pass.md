# A render is resized after geometry, in one separable pass

[ADR 007](007-camera-primaries-travel-with-the-pixels.md) put a target size on
`RenderRequest` from the start, and left it unimplemented until there was a
`--resize` flag. That ADR decided:
- the request is fitted after the crop;
- it only shrinks unless upscaling is allowed;
- the flag takes three forms;
- the pipeline, not the caller, decides where to resample.

[ADR 011](011-a-plan-spatial-passes-and-one-pointwise-chain.md) put the target
size in a resample block at the end of the plan. [ADR 014](014-rotation-straightening-and-crop-settings.md)
left "requested target-size resizing, including its antialiasing policy" out
of the geometry pass. The [target-size plan](../ideas/target-size-plan.md)
implemented it on the CPU and the GPU, and for the command line, Python and
the window. This ADR records how a render reaches its size.

## Decision

**A request names a box or a factor, and the pipeline resolves it.**
`RenderRequest::size` is `RenderRequest::FitInside{width, height}` or
`RenderRequest::Scale{factor}`. A long edge of N is `FitInside{N, N}`. Both
forms are resolved against the size after the crop, which only the pipeline
knows, in one public function, `resolvedSize`:
- a factor is used as given;
- a box gives the smallest factor that fits both sides;
- the factor is capped at 1 unless the request says `Upscale::Allowed`;
- each side is rounded and is at least 1 pixel.

The types are nested in the request because outside it "a scale" means
nothing.

**The size is part of the plan.** `planFor` resolves the request into
`ProcessingPlan::resize`: output size, filter, and an opacity hint. That block
is the `Resize` group of `stagesOf`. So two renders at different sizes or with
different filters differ at `Stage::Resize`, and a box and a factor that reach
the same size are the same plan (ADR 012). An identity resize, where the
output equals the cropped size, carries no filter and costs nothing on either
backend. Both backends read the size from the plan and never resolve it
themselves.

**Resizing is a separate pass after geometry.** Geometry keeps its exact
bilinear inverse mapping (ADR 014). The resize that follows is separable:
- a pass over rows, then a pass over columns;
- weights precomputed once per output column and row;
- linear working colour and premultiplied alpha, as in geometry.

Each part can be tested alone, and the GPU runs the same algorithm. The cost
is that a small render still pays for a full-resolution geometry pass. That is
the price of exactness, and what a preview path will avoid (below). Folding
the scale into the geometry pass was rejected: a rotated, scaled footprint is
not separable, costs a number of taps that grows with the square of the
reduction, and is much harder to make agree across backends.

**Two filters.**
- **`ResizeFilter::Lanczos3`, the default,** is sharp, with some ringing at hard
  edges.
- **`ResizeFilter::Bilinear`** is a tent: soft, and never rings.

When shrinking, both kernels are widened by the reduction factor, so neither
aliases. When enlarging, Lanczos is Lanczos and the tent is ordinary bilinear.
The output centre maps to `(x + 0.5) / scale − 0.5` in the source, and taps
beyond the edge repeat the edge pixel. Weights are normalised to sum to 1.

**Ringing never makes black darker, and nothing else is clamped.** Lanczos has
negative lobes. After each 1-D pass, a channel is clamped at 0 when every input
that fed it was at least 0. So a halo cannot go below black, while the genuine
negatives a camera matrix produces survive. Overshoot above the bright side of
an edge is kept: the shoulder has already run, and export clips. A clamp to the
range of each window's colours ("anti-ringing") was rejected for opaque
images. It would make the filter non-linear everywhere for a modest visual
gain.

**Transparency is the one exception.** With signed weights, premultiplied
colour and alpha are different sums, and their quotient is unbounded next to a
transparent area: it reached 620 from inputs in [0, 1]. So:
- alpha is clamped to [0, 1];
- alpha below 2⁻¹⁶ is fully transparent;
- where a window holds any pixel that is not opaque, colour is clamped to the
  range of that window's visible colours.

Opaque windows are untouched.

**Opaque stays exactly opaque, on every backend.** A source is opaque when it
has no alpha channel, or when every alpha sample is exactly 1.0. That is
scanned once, only for a resize that runs, and it stops at the first sample
that is not 1. Geometry keeps an opaque source opaque. The CPU geometry
sample stores a double weight sum that rounds to exactly 1. The GPU geometry
and resize shaders set alpha to 1 outright when every contributing pixel is
opaque, instead of trusting float weight sums. Exactly 1.0 matters in two
places: the CPU's translucency test, and JPEG export, which refuses a buffer
that is not opaque.

Opacity is an execution hint, not part of plan equality, because a plan made
from a `Photo` has no pixels to scan. It selects a fast path that tracks no
colour range and makes no final clamp:
- **On the CPU** the fast path is bit-identical to the general one, by
  construction and by test.
- **On the GPU** it is one row render and one column render with one
  intermediate texture, instead of four renders and three intermediates.

**The GPU matches the CPU by sharing its weights.** The CPU computes the
weights in double and uploads them, one table per axis. They are not computed
in the shader, because a float sample position at x ≈ 6000 is only good to
about 5·10⁻⁴ pixel, which shows at sharp edges. This also keeps the kernel
formulas in one place.

On lavapipe the GPU agrees with the CPU:
- to 7.8·10⁻⁷ in colour on well-conditioned input;
- to 1.7·10⁻⁶ with negatives and values above 1;
- to 3.6·10⁻⁷ in alpha.

The general path runs the row stage once per intermediate (sums, low, high),
because one render writes one output. Multiple render targets would cut that
to one render.

**Quality is in the request, and only Export exists.** `RenderRequest::quality`
has one value, `Quality::Export`: full resolution all the way. A `Preview`
quality will come with the GUI's editing loop: a pyramid of 2× box reductions
of the developed image before geometry. It will be checked against Export,
with a looser tolerance. It changes pixels, so when it lands `quality` joins
the plan's resize block and its equality.

**Every caller uses the same request.**
- **`arraw-cli export`** takes ADR 007's `--resize 2048 | 2048x1365 | 12.5%`,
  with `--allow-upscale` and `--resize-filter lanczos|bilinear`. Either of the
  last two without `--resize` warns that it does nothing.
- **Python's `develop`** takes `size=` as an int long edge, a `(w, h)` box or a
  float factor, with `filter=` and `allow_upscale=`.
- **The window** develops the open photograph at the view's size in device
  pixels. It is never enlarged, so a small photograph shows at 100%, and it
  re-renders when the view's size or pixel ratio settles.

## Consequences

- **A preview and an export are the same call.** What the window shows at
  2048 pixels is what `--resize 2048` writes, on either device.
- **Changing only the output size reuses everything before the resize,** once
  a cache exists. The `Resize` stage is last and compares only size and
  filter.
- **Large reductions cost a full-resolution geometry pass, and on the GPU's
  general path three full-size float intermediates** (about 390 MB for 24 MP
  down to 2048). Opaque photographs take the fast path, with one intermediate.
- **`GpuContext::renderCount()`** exists so that tests can see which path
  ran.

Conditions that require revisiting this decision:

- **The GUI's editing loop measures a slider tick as too slow.** `Preview`
  quality and its pyramid come in then, and `quality` moves into the plan.
- **Output sharpening, grain or noise reduction become scale-aware.** ADR 007's
  reason for resizing inside the pipeline. They will need the resolved scale
  from the resize block.
- **A setting starts producing transparency** (a vignette with alpha, a
  masked export). The opacity hint must then come from the developed image,
  not the source.
