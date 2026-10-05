# Renders stop and resume at pass boundaries

[ADR 011](011-a-plan-spatial-passes-and-one-pointwise-chain.md) decided that a
render can stop after a pass boundary and resume from the checkpoint it left.
A checkpoint stays valid as long as the new plan equals the old one up to that
boundary. [ADR 015](015-a-checkpoints-pixels-may-live-on-a-device.md) made the
checkpoint real, with pixels on the host or on a device. `stopAfter` and
`resumeFrom` waited for a caller that renders the same photograph repeatedly.
The GUI preview is that caller
([ADR 023](023-a-preview-develops-a-reduced-copy-of-the-source.md)).

## Decision

**Both backends stop and resume through one path each:**
- The CPU has `developUntil(source, state, stopAfter, request)` and
  `resumeFrom(checkpoint, source, state, stopAfter, request)`.
- The GPU has a `developOnGpu` overload that takes a resident checkpoint.
- `develop` keeps its direct path, which takes no checkpoint and copies
  nothing.
- On each backend, a fresh render and a resumed one run the same stage
  functions. The GPU's existing `developOnGpu` now goes through that shared
  path too.

**The engine decides validity in one place, `requireResumable`.** A resume is
refused with `std::invalid_argument` when:
- the plan prefix up to the checkpoint's boundary differs;
- the render would stop before that boundary;
- the checkpoint's pixels are not the size the new plan expects at that
  boundary. A pointwise checkpoint comes before any geometry, so its prefix
  cannot tell a reduced copy from the source; the size check can.

Callers never read a plan. A checkpoint resumes only where its pixels live: a
host one on the CPU, a resident one on its own device. A host checkpoint is not
uploaded for the GPU, because holding resident checkpoints exists to avoid that
transfer.

**Source identity is the caller's job until the plan has a decode block.** The
plan carries the source's size, orientation and encoding, but not a file or a
stamp ([ADR 012](012-a-render-resolves-from-a-photo-and-a-request.md)). Two
different sources with the same size and encoding cannot be told apart. A
caller that swaps one for the other must drop its checkpoints. The preview
does: its cache belongs to one pyramid level's buffer and is cleared when the
buffer changes.

**The preview keeps the last pointwise and geometry checkpoints of the level
it shows:**
- it tries the geometry checkpoint first, then the pointwise one, then a fresh
  develop;
- every render stops at each boundary to refresh the cache, then carries on to
  the resize;
- a checkpoint the engine refuses is stale and is dropped. The preview tells a
  stale checkpoint from a bad request by retrying: a bad request fails again
  in the fresh develop and is reported.

The effect:
- a window resize resumes after geometry;
- a straighten or crop edit resumes after the tone and colour stage;
- a tone edit starts again from the level.

`PreviewResult::resumedFrom` reports which, for tests and diagnostics.
Checkpoints are released on the preview thread, as the device requires.

## Consequences

- **On the GPU a resume is free.** Checkpoints share their device images, so
  stopping at each boundary costs no extra passes.
- **On the CPU a resume copies.** The stage functions take their input by
  value and checkpoint pixels are immutable, so each resume clones the stored
  pixels. A tone tick on the CPU fallback therefore pays two extra copies of
  about the level's size. Letting the stages read borrowed input would remove
  them, and is worth doing if the CPU fallback is measured as too slow.
- **The cache pays off more as the pipeline grows.** With three cheap stages at
  preview size the saving is modest. Lens correction, spots and noise reduction
  will come before tone and colour, so a slider tick will then resume after
  them, as main's cached derivative buffers did.
- **A decode block in the plan** will move source identity into the engine. The
  preview's per-buffer binding can then go.

## Note, 2026-10-04

A fourth boundary, `Stage::Effects`, follows the resize
([ADR 037](037-effects-run-after-the-resize-and-the-vignette-follows-the-crop.md)).
Renders end there, and the preview keeps a resize checkpoint beside the
pointwise and geometry ones, so a vignette edit resumes after the resize. With
every effect off the boundary collapses onto the resize.

## Note, 2026-10-05

A first boundary, `Stage::Denoise`, comes before the pointwise pass
([ADR 039](039-noise-reduction-is-the-first-pass-and-reads-the-as-shot-luminance.md)):
a tone or white balance edit with noise reduction on resumes after it, and
pays only the chain (the chain reads the checkpoint without copying it). With
noise reduction off the boundary collapses onto the source and the preview
keeps no checkpoint there.
