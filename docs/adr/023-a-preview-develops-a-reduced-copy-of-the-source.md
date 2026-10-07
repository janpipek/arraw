# A preview develops a reduced copy of the source

[ADR 020](020-a-render-is-resized-after-geometry-in-one-separable-pass.md) left
one quality, `Export`, which works at full resolution all the way. It promised
a `Preview` quality "when the GUI's editing loop measures a slider tick as too
slow", built as "a pyramid of 2× box reductions of the developed image before
geometry".

The editing loop now exists ([ADR 022](022-an-edit-is-begun-updated-and-committed-as-one-step.md)).
It renders on its own thread, on the GPU when there is one, from a source
uploaded once. A full-resolution develop per slider tick is still the cost:
- on the CPU fallback it takes seconds for a 24 MP RAW;
- on the GPU it means several full-size float intermediates per frame.

Main's preview worked from a half-size copy of the decoded image, made at load,
and developed everything from it. Full resolution was used only for export and
close zoom.

## Decision

**The preview develops a reduced copy of the decoded source, made before any
development.** `halved` averages 2×2 blocks:
- premultiplied, so a transparent pixel does not darken its neighbours;
- the result is RGBA float, keeping the input's encoding and orientation;
- odd sizes round up, and the last row or column averages only the samples it
  has.

Repeated halving gives a pyramid: level 0 is the source, and each level halves
the one before.

This puts the reduction before tone and colour, not after them as ADR 020 had
it. Reducing after tone and colour would keep tone exact, but it saves only the
geometry and resize passes. The CPU fallback would stay slow, and the GPU would
still develop full size. Tone and colour applied to averaged pixels differ from
export only at fine, high-contrast detail. `tests/test_ImagePyramid.cpp`
measures how far. A 1200×800 fixture of gradients and fine detail, developed
from level 2 into 300 pixels, differs from the full-resolution develop by a
mean of 0.0024 and at most 0.011 in linear working units. The test bounds are
0.005 and 0.02. One-pixel hard edges would differ more, and the fixture has
none.

**The level is the smallest that still covers the output.**
`pyramidLevelFor(sourceSize, orientation, state, request)`:
1. resolves the cropped size through the geometry plan, at full resolution;
2. resolves the output size through `resolvedSize`;
3. picks the deepest level whose cropped size is at least the output size on
   both sides.

The preview therefore develops at most twice the pixels it shows, on each side.
Halving rounds up, but the crop of a reduced copy is floored, so a preview can
come out a pixel smaller than the full-resolution render would.
A request needing full detail, such as an upscale, a photograph smaller than
the view or, later, a close zoom, gets level 0. The engine owns this choice,
because only the engine knows the geometry plan.

**No `Quality` field is added to the request.** The caller chooses which buffer
to develop, and the rest of the pipeline is unchanged. Normalised crop
coordinates frame a reduced copy exactly as they frame the source. A different
source makes a different plan, so checkpoints from two levels can never be
mistaken for each other ([ADR 011](011-a-plan-spatial-passes-and-one-pointwise-chain.md),
[ADR 012](012-a-render-resolves-from-a-photo-and-a-request.md)).

**Export stays exact.** Only the preview thread uses the pyramid. The command
line and Python develop the source they are given.

**The preview thread builds levels lazily, per source:**
- a level is made from the previous one the first time a request needs it;
- no level is made below a long edge of 256 pixels;
- on the GPU each level is uploaded the first time a request uses it. Level 0,
  the full-size upload, happens only if a request needs it.

Everything belongs to the source given to `setSource`. A new source starts a
new pyramid. A GPU failure on any level, for example a full-size upload beyond
the device's texture limits, sends that photograph to the CPU for every level.
Narrowing that to the failing level is possible once close zoom needs level 0.

**The way stays open for a picture before decoding finishes.** Main showed a
RAW's embedded JPEG while the RAW decoded. That needs decoding off the
interface thread, which is deferred. Because the pyramid hangs off whatever
source the preview thread was given, a provisional picture can be added later
as a different kind of source without reshaping the levels.

## Consequences

- A slider tick costs a develop of roughly the viewport's pixels, on either
  device. That is about 1/16 of a 24 MP photograph shown at 1500 pixels.
- What the window shows is not bit-for-bit what an export at the same size
  writes. ADR 020's "a preview and an export are the same call" now holds for
  the call, not for the pixels.
- Close zoom will need level 0, and on the GPU a full-size upload, the first
  time it is used. Main did the same.
- When spatial stages arrive (lens correction, spots, noise reduction), each
  works on the chosen level. Any radius in pixels must scale with the level,
  which ADR 020 already anticipates for scale-aware stages.

## Note, 2026-10-05

The level is now known from the pixels: `halved` doubles
`ImageBuffer::pixelScale`, and noise reduction divides its radii by the
source's scale ([ADR 039](039-noise-reduction-is-the-first-pass-and-reads-the-as-shot-luminance.md)). The preview, the curve histogram and the thumbnail no
longer tell the render which level they develop, and a half-size decode counts
as a reduction of 2.

## Note, 2026-10-06

Clarity and Dehaze are relative to the long edge and their grid keeps its
sensor-pixel cell on a pyramid level, so a level-2 preview shows within 0.2%
of the effect the full render does. Texture is in sensor pixels and held at
one pixel of the level, so a preview shows it coarser than 1:1
([ADR 041](041-texture-clarity-and-dehaze-read-a-context-of-log-luminance.md)).
