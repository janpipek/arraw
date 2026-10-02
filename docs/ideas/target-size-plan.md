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
| Quality | `RenderRequest::quality = Quality::Export`, the only value implemented. `Preview` (a pyramid early downsample) comes with the GUI loop, checked against Export |
| GPU | In the same step: two shader passes after the geometry pass. Parity with the CPU is tested as for geometry |

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
