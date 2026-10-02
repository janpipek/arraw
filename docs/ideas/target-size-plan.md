# Rendering to a requested size — execution plan

Status: proposed.

## Goal

`develop(source, settings, request)` renders the photograph, after its crop,
at a requested size. The CPU and the GPU give the same result. The command
line (`--resize`), Python (`develop(size=…)`) and the GUI preview all use it.
This is the last part of `RenderRequest` that throws "not implemented".

Not in scope:
- a fast preview path (an early downsample through a pyramid);
- output sharpening;
- region rendering.

## Decisions

| Question | Answer |
|---|---|
| What a request says | `size` is `std::variant<RenderRequest::FitInside{width, height}, RenderRequest::Scale{factor}>`. A long edge of N is `FitInside{N, N}`. Both forms are resolved by the pipeline against the size after the crop (ADR 007) |
| Upscaling | `Upscale::Never` by default, so the scale is capped at 1. `Allowed` lifts the cap |
| Output size | `round(cropped × scale)` per side, at least 1 pixel. For `FitInside`, the scale is the smallest that fits both sides |
| Filter | `ResizeFilter::Lanczos3` (default) or `ResizeFilter::Bilinear`. Bilinear is a tent kernel widened by 1/scale when shrinking, so it does not alias; when enlarging it is plain bilinear |
| Where | A separate final pass after geometry, made of two 1-D passes (rows, then columns) with weights precomputed per output row and column. Geometry is unchanged (ADR 014). This follows ADR 011's "a target size to the resample block at the end" |
| Colour and alpha | Linear working colour, premultiplied alpha, as in the geometry pass |
| Ringing | After each 1-D pass, a channel whose input taps were all ≥ 0 is clamped at 0. So Lanczos halos cannot go below black, while the genuine negatives from the camera matrix survive. Values above 1 are kept: the shoulder has already run, and export clips |
| Transparency | Lanczos weights are signed, so near transparency the colour/alpha quotient is unbounded. Alpha is clamped to [0, 1], and alpha below 2^-16 is fully transparent. Only where a window holds a pixel that is not opaque, colour is clamped to the range of that window's visible colours. Opaque windows keep plain filter output, overshoot included |
| Identity | A resolved size equal to the cropped size does no resize, and the buffer is reused |
| Quality | `RenderRequest::quality = Quality::Export`, the only value implemented. `Preview` (a pyramid early downsample) comes with the GUI loop, checked against Export. Once Preview lands it changes the pixels, so `quality` must move into `ResizePlan` (and its equality); today it does not reach the plan |
| Plan block | The request is resolved into the plan (ADR 012), in `ProcessingPlan::resize`: the output size from `resolvedSize` against the geometry's output size, the filter, and whether the source is opaque (ADR 011 puts the target size in the resample block). `stagesOf` has it as the `Resize` group, so `prefixMatches(.., Stage::Resize)` tells requests apart, and a box and a factor that reach one size are the same plan. Identity is representable (output size equals the cropped size) and both backends skip the work. An identity block carries the default filter and `opaque = false`, since no kernel runs. `develop` and `developOnGpu` read size and filter from the plan alone; `developOnGpu` plans with the default request when it stops before the resize, so that no scan is paid for a request it ignores. A `Photo` has no pixels, so `planFor(photo, request)` never marks a resize opaque |
| Opaque fast path | Opaque means no alpha channel (an `Rgb*` format, free) or every alpha sample exactly 1 after `toUnit`, scanned once on the host in `planFor(source, settings, request)`, only for a resize that runs, stopping at the first sample that is not 1 (NaN, above 1 and one step below 1 all count as not opaque). The cost is a read of the alpha samples, at worst the whole buffer (a strided read, bounded by memory bandwidth; not measured). Geometry keeps an opaque source opaque: it only blends source pixels (edge pixels repeat beyond the edge, ADR 014's crops stay inside content), and both backends give alpha exactly 1 for opaque neighbours (`sample()` stores the double sum as float; `geometry.frag` sets 1 outright). CPU: `resample(.., opaque)` tracks no low, high or translucency and does no final colour clamp; the result is bit-identical to the general path on opaque input (tested). GPU: separate passes `ResizeAcrossOpaque` and `ResizeDownOpaque` |
| GPU pass count | General: the horizontal stage runs three times (sums, low, high planes) and the vertical once, four renders and three full-size intermediates. Opaque: one horizontal render writing the sums plane and one vertical render writing colour with alpha exactly 1.0, two renders and one intermediate. Both read the CPU's weights, uploaded once per axis. Identity runs none. `GpuContext::renderCount()` makes the count testable. Follow-up for the general path: multiple render targets, to cut it to two renders |
| GPU | In the same step: two 1-D shader stages after the geometry pass, as above. Deviation: one render writes one output, so the general path runs the horizontal stage once per plane. Parity with the CPU is tested as for geometry, for both paths; the opaque passes agree bit for bit with the general ones on an opaque image (measured on lavapipe) |

## Steps

1. **Core on the CPU.** This step adds:
   - the `RenderRequest` shape (size variant, `ResizeFilter`, `Quality`);
   - `resolvedSize(request, croppedSize)` as the single place sizes are
     worked out;
   - a private resample module (`src/core/Resample.*`), and `develop()`
     wiring.

   Tests cover:
   - the size table for every form, with and without upscaling;
   - a constant image staying constant;
   - weights summing to 1;
   - no aliasing on a zone plate or fine stripes when shrinking 8×;
   - the clamp rule;
   - premultiplied alpha, with no fringes;
   - identity reusing the buffer;
   - enlarging.
2. **GPU.** This step adds the two 1-D shader passes after geometry, wired
   into the GPU develop path. Parity tests use both filters, several scales,
   alpha and negatives. ADR 017's device rules apply unchanged.
3. **Callers.** This step adds:
   - on the command line, ADR 007's `--resize 2048 | 2048x1365 | 50%`,
     `--allow-upscale` and `--resize-filter lanczos|bilinear`;
   - in Python, `develop(…, size=2048 | (w, h) | 0.5, filter=…, allow_upscale=…)`;
   - in the GUI, the photo shown at window size through the request, on the
     CPU on the exact path.
4. **ADR 020** records the request shape, the filters, the clamp rule and the
   quality policy. ADR 007 and ADR 014 get notes pointing to it.

Execution follows the earlier plans: small workflows with Sonnet implementing
and Opus reviewing, with a review between steps.

For ADR 020 (step 4): opaque input must give exactly 1.0F alpha on every backend, since the CPU translucency key and the JPEG export's isOpaque both rely on it. The GPU geometry and resize passes therefore set alpha to 1 when every contributing pixel is opaque, rather than trusting float weight sums.
