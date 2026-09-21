# Development is a plan, a few spatial passes, and one pointwise chain

`develop` is currently a single fused pass: resolve one matrix, multiply every
pixel, done. Tone, colour, spots, lens corrections and noise reduction are all
still to come, and the obvious next move — one `applyX` buffer-to-buffer
function per setting, called in order — would be the wrong one. Ten pointwise
adjustments as ten passes is ten traversals of a gigabyte and nine intermediate
buffers, and on the GPU nine extra render targets, to compute something that is
one arithmetic expression per pixel.

## Decision

**Stages are of two kinds, and the difference decides everything else.**

*Pointwise* stages depend only on the pixel and, sometimes, its coordinate:
white balance, the camera matrix, exposure, tone, HSL, the Oklab colour
controls, grading, black and white, vignette, grain. They fuse into one pass,
and the first three fuse further into a single 3x3 and a scalar.

*Spatial* stages need neighbours or a different geometry: demosaic, chromatic
aberration, distortion, spots, noise reduction, sharpening, crop and resample.
Each is genuinely a pass, `ImageBuffer` to `ImageBuffer`, and each names its
input and output reference frames per ADR 009.

**A `ProcessingPlan` is computed once per photograph and both backends consume
it.** It holds everything the settings imply — the composed matrix, the
exposure gain, later the tone curve and colour coefficients. This is ADR 009's
rule, stated there for geometry, applied to the whole pipeline: no backend
derives anything from `DevelopSettings` itself. The plan is also, in time, the
GPU's uniform block.

**The order lives in one function.** `developPixel(plan, context, colour)` is
the fixed sequence as a pure function of one pixel, so the order is readable in
one place and testable without a buffer. The loop over the buffer is then too
small to hide anything, and the fragment shader's `main` is a hand-written
mirror of the same sequence, held to it by per-stage comparison tests rather
than by generation — as ADR 009 chose, and as `main`'s `image.frag` is already
built.

**The plan carries what is the same for every pixel; the context carries what
is not.** Pointwise does not mean colour-only: a vignette darkens the edge and
not the centre, and grain is a pattern in space, both of which ADR 009 gives
composition-frame coordinates. So the context holds the pixel's position in
each frame the chain reads — the buffer, the image and the composition — and
everything per-photograph stays in the plan: the aspect a mask shape needs, a
grain seed, mask geometry. `main`'s fragment shader arrived at the same split,
taking `vUV`, `vImageUV` and `vFrameUV` as varyings while reading `aspect` from
its uniform block. A stage can then be held to the rule rather than to taste,
and the chain stays pure and fusible.

**The change from scene-linear to perceptual encoding is a stage in that
sequence**, not something each operation does for itself. ADR 010 puts exposure
and white balance in scene-linear and tone and colour in a perceptual
coordinate; naming the crossing keeps every stage's input unambiguous, and
keeps the two encodings from being converted back and forth per control.

**Intermediate values are read back by asking for them, not with callbacks.**
A caller names where to stop and receives what is there. It is one mechanism
that behaves the same on both backends — the GPU branches on a uniform, which
is what `main` does for its curve-input histogram. An event and handler
interface would fire naturally on the CPU and have to be faked on the GPU,
where the intermediate values never exist as a buffer; that is the divergence
ADR 009 exists to prevent. Writing several taps in one pass, through multiple
render targets, is the optimisation to reach for when a measured frame cost
asks for it.

**Looking and continuing are two verbs.** `sample(tap)` hands back pixels to
measure; `stopAfter(boundary)` hands back a `RenderCheckpoint` to carry on
from. They are different questions with different answers, and one call
returning both — pixels, plus a checkpoint that may or may not be there — would
have a return type whose meaning depended on an argument.

A *tap* is a named position inside the pointwise chain, declared beside
`developPixel` so that the name and the position cannot drift, and each one
documents the encoding it hands back. The first is the curve input: after white
balance, the matrix, exposure and the crossing into the perceptual coordinate,
immediately before the tone curve, which is where ADR 010 puts a curve widget's
x-axis. `main` has exactly this, as `curveInput` — "stop after Basic Tone +
gamma-encode". Its second tap needs no equivalent here: `wbInput` stops
*before* white balance, and the start of the pointwise chain is the end of the
pass before it, so a white-balance picker reads a boundary rather than a tap.

**A pass boundary is a place where a buffer exists anyway.** Every spatial
stage is a pass, and the fused pointwise chain is one too, so the pixels at a
pass's end exist whether or not anyone asks for them; keeping one costs a
reference rather than a traversal. Inside a fused pass there is no buffer at
all — a colour is in registers between exposure and tone — which is why a tap
is not a checkpoint. Which boundaries exist is a property of the resolved plan
rather than of the stage list: a stage whose setting is off is not a pass, and
its boundary collapses onto the one before it.

**Out-of-band information does get a sink.** Warnings, progress and
cancellation are about the operation rather than the image, carry no pixels,
and behave identically on both backends, so they travel on a caller-supplied
channel. Its first customer already exists: a RAW that declares no white
balance is decoded with a substituted illuminant, and `RawImport.cpp` has been
carrying a comment asking for somewhere to say so.

**A render can resume, and what it resumes from carries its own provenance.**
`stopAfter` produces a `RenderCheckpoint` — pixels, the boundary they were
taken at, and the part of the plan that made them — and a request can
`resumeFrom` one. Not a boundary number: handing over a bare buffer and a stage
is how a stale render happens, silently and in colour. Only pass boundaries are
checkpoints, for the reason above. ADR 012 says what a plan is resolved from,
and therefore what a checkpoint's provenance amounts to.

Provenance stays on the checkpoint rather than on the buffer. `ImageBuffer` is
a plain raster by ADR 001, and one that knew which pipeline made it would be
the partially populated state that ADR forbids.

**A checkpoint is validated by comparing plans, not by hashing them.** A stored
copy of the plan and `==` is exact, needs no hash function, and cannot drift
from what actually executes; next to a buffer of hundreds of megabytes, a few
hundred bytes of plan is free. Float equality is sound here because these are
values our own code derived deterministically from the same inputs, not
measurements — and the failure direction is safe, since a NaN never compares
equal and simply recomputes. Large payloads, such as a Brush raster or a long
Spot list, are compared by revision rather than by value.

This only works if the plan genuinely holds everything the stages consume. That
is the same discipline ADR 009 states for transforms, and comparing plans is
what makes a violation fail rather than merely be untidy.

**Every input a stage consumes sits in that stage's block** — whether it came
from the settings, from the photograph's metadata, or from the render request.
A decode resolution belongs to the decode block, a noise-reduction quality to
the noise block, a target size to the resample block at the end. Then a
preview's early downsample cannot satisfy a full-quality export, while changing
only the output size reuses the whole prefix before it — both by construction,
with no case to write. Policies resolve away: a block holds the concrete
numbers its stage executes, never a "draft" for something later to interpret,
for the same reason ADR 009 has the plan hold transforms rather than the
settings that imply them.

**The plan is partitioned by stage, and prefixes are compared up to a point.**
Comparing whole plans would defeat the purpose: nudging Exposure would discard
a noise-reduction result that does not depend on it. So the plan holds one
block per stage, `stagesOf` lists them in pipeline order, and the comparison is
a fold that stops at the checkpoint's stage:

```cpp
constexpr auto stagesOf(const ProcessingPlan& p) {
    return std::tie(p.decode, p.lens, p.spots, p.noise, p.pointwise);
}

bool prefixMatches(const ProcessingPlan& a, const ProcessingPlan& b, Stage after) {
    const auto left = stagesOf(a), right = stagesOf(b);
    const auto last = static_cast<std::size_t>(after);
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return ((I > last || std::get<I>(left) == std::get<I>(right)) && ...);
    }(std::make_index_sequence<stageCount>{});
}
```

There is no per-stage line to forget, and the list itself is guarded twice: a
`static_assert` that the tuple's size equals the number of stages, and, under
`__cpp_reflection` on the one compiler that has it, that the plan's field count
does too. That is the arrangement ADR 008 already uses for the descriptor
table.

That partition also makes structural a property currently held by a comment:
ADR 007 has the demosaic use the as-shot multipliers so that the temperature
slider does not invalidate the most expensive cache in the program. With the
as-shot gains in the decode block and the balanced matrix in the pointwise
one, changing a temperature *cannot* reach the decode prefix.

**Two verbs, deliberately.** *Develop* is the photographic act a photographer
performs on a photograph; *render* is producing pixels from it. So
`DevelopSettings` and `develop()` on one side, `RenderRequest` and
`RenderCheckpoint` on the other, and the reimplementation plan's "render
request" keeps its meaning.

## Consequences

- **Each tap costs a render.** Two histograms from different points in the
  chain means three renders, until multiple outputs are implemented.
- **The pointwise chain cannot be inspected between stages for free**, which is
  the price of not paying for nine intermediate buffers. `sample` buys back
  exactly the points that are asked for.
- **A sampled buffer declares its own encoding, and one of them has no name
  yet.** `ColorEncoding` carries the meaning of an `ImageBuffer`'s samples, but
  `NamedEncoding` lists only linear Rec.2020, sRGB, Display P3 and Adobe RGB.
  The curve input is perceptual (ADR 010), so the stage that crosses into that
  coordinate brings a name with it, and the tap is self-describing rather than
  a buffer whose numbers mean something only its caller knows.
- **`developPixel` must stay branch-light.** It runs per pixel, so settings
  that are off should fall out in the plan rather than as a test inside the
  loop.
- **`sample`, `stopAfter` and the diagnostics sink are decided here and
  implemented with their first callers** — the histogram, the resuming
  processor and the import warning respectively. None is added as an unused
  field, and `developPixel` likewise gains its context when a stage first reads
  one.
- **`resumeFrom`, `RenderCheckpoint` and the stage-partitioned plan are decided
  here and built with the processor that caches them** — ADR 007's promotion
  path, where callers stop resuming by hand and simply call `render` again.
- **A stage's kind is part of its definition.** Anything pointwise that later
  needs a neighbour, such as clarity or a local contrast, becomes a spatial
  stage and a pass of its own rather than being smuggled into the chain.
