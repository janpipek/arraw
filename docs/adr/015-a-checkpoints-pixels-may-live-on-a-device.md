# A checkpoint's pixels may live on a device

The [GPU implementation plan](../ideas/gpu-implementation-plan.md) proposed
`Renderer::render(ImageBuffer, ProcessingPlan) -> ImageBuffer` as the backend
boundary. Its [review](../reviews/gpt-6_2026-09-22_gpu-implementation.md) showed
that interface cannot deliver a preview: every call would read a
full-resolution texture back to the host and upload it again, and a viewport
that wanted to present the texture would have to reach around the interface
through something nobody had described. The reimplementation plan already
forbids exactly this — §12 asks to "keep pixels on the GPU across compatible
passes and, where possible, through viewport presentation" — so the proposal
contradicted a requirement we had already written down.

That undescribed second path is the whole difficulty, because inventing it later
is not free. It has to say who owns the graphics device, what a retained result
*is*, and how a stale one is refused, and those answers reach into
`ProcessingPlan`, into ADR 011's checkpoints, and into the target graph. Three
shapes were argued; the last section here records the two that lost.

## Decision

**A retained GPU result is a `RenderCheckpoint`, not a new kind of thing.** ADR
011 already defines one as pixels, the boundary they were taken at, and the part
of the plan that made them, produced by `stopAfter` and consumed by `resumeFrom`.
A developed texture waiting for a geometry edit is that, exactly. The argument
that decides it is not the GPU's: ADR 011 spent its length establishing that
reuse is validated by comparing plan prefixes, and any second retention concept
creates a place where that rule gets re-implemented by whoever is in a hurry.
Adopting the noun we have costs one opaque type and buys the rule, its guards,
and the CLI's access to both.

**The payload is where the backends differ, and deliberately the only place.**

```cpp
using CheckpointPixels = std::variant<ImageBuffer, DeviceImage>;
```

Everything else about a checkpoint — its boundary, its plan, how it is
validated, what it means — is one description that both backends answer to. That
is ADR 009's rule about divergence applied to residency: a mechanism that
behaved naturally on one backend and had to be faked on the other is what these
ADRs keep refusing.

**A device image carries the device that holds it.** QRhi resources belong to
the instance that made them, are used from that instance's one owner thread, and
are not shareable between instances, so a preview viewport and an offscreen
exporter are two devices and a texture from one is meaningless to the other.
Carrying `DeviceId` next to the texture makes handing it to the wrong device a
refusable mistake rather than a convention someone is asked to remember. The
reimplementation plan's §12 already requires each context to have a defined
owner thread; this is what that looks like in a type.

**The plan prefix is not reachable from a checkpoint.** Not encapsulation for
its own sake: a caller that could read the prefix out would decide validity for
itself, at each call site, which is the failure this ADR exists to prevent. The
engine compares; callers hold.

**Reading back is a named cost, never an accessor.** `readBack()` always copies —
a device transfer from a resident checkpoint, a clone from a host-resident one,
because the payload is shared and immutable. `size()` and `encoding()` are cheap
and answer without touching pixels, so describing a checkpoint never accidentally
moves a gigabyte. Export, diagnostics and the CPU/GPU comparison tests are what
readback is for; a preview presents instead.

**Pass-recording code takes a device it does not own; adapters own one.** Code
that owned its device could never render into the viewport's, and code that only
ever used a supplied one could not serve the CLI, which has no viewport. So the
ownership rule is written as an access rule: `DeviceImage`'s constructor is
private and only a `GpuContext` may mint one.

**Qt stops at `src/gpu/*.cpp`.** `DeviceImage` holds its QRhi texture behind a
`struct Impl` defined in the translation unit, so its own header names no
graphics type, `src/core` may hold a device image, and `include/` stays free of
Qt as it is today. This is not tidiness: the library and the CLI must be able to
render without depending on application or graphics headers, and `Qt6::GuiPrivate`
has weaker compatibility guarantees than public Qt, so the number of translation
units that can be broken by a Qt minor release should be as small as we can make
it.

**`Stage` names the pass boundaries that exist, not the ones that will.** Today
that is `Pointwise` and `Geometry`. ADR 011 sketches a five-block partition —
decode, lens, spots, noise, pointwise — but the plan is still flat, so
`stagesOf` groups fields rather than blocks and grows into that sketch as each
block arrives. A boundary list that named passes nobody had written would make
`prefixMatches` return answers about nothing.

**Only what has a caller is built.** ADR 011's discipline, applied here: the
checkpoint, its payload and the prefix fold exist because this decision needs
them to be real; `GpuContext`, the pass recording and the readback transfer
arrive with the first GPU pass. `DeviceImage` can therefore only be empty today,
and that is the correct amount of GPU code to have written before a probe has
established that the backends we claim to support can read a float texture back
at all.

## Consequences

- **ADR 011's `stagesOf` snippet is superseded in shape, not in intent.** The
  fold, the `static_assert` on the tuple's size and the argument for exact float
  comparison all stand; the five blocks become two groups until the blocks are
  real. See the note there.
- **The field-count guard ADR 011 asks for is not in place.** It specifies a
  `__cpp_reflection` assert that the plan's field count matches the tuple's, and
  no compiler we build with has that yet. Until one does, a field added to
  `ProcessingPlan` and forgotten in `stagesOf` compiles silently, and the guard
  is a test asserting that a full-depth `prefixMatches` agrees with `operator==`.
- **Two devices means two checkpoints.** A preview cannot hand its developed
  texture to an export running on another device; the export renders its own, or
  resumes from a host-resident checkpoint. Sharing textures between contexts is
  a separate decision with its own synchronisation, and nothing here assumes it.
- **`ImageBuffer` is unchanged.** ADR 001 keeps it a plain raster and ADR 011
  puts provenance on the checkpoint; a device image is a payload beside a buffer,
  never a state inside one.
- **The stale-texture hazard the review raises is structural now, not a rule.**
  Its finding 3 is right that `planFor(encoding, settings)` does not identify the
  source pixels, so two photographs can resolve equal plans. ADR 012 already
  fixes that by putting the file and a stamp of its size and modification time
  in the decode block. The consequence for this work is narrow and concrete: no
  cache may be keyed on a plan resolved from the narrow overload.
- **Texture precision is not decided here.** Whether storage is RGBA32F or
  RGBA16F changes pixels — half-float storage alters alpha and loses values the
  CPU chain deliberately preserves above white and below zero — and it is an
  empirical question about what the hardware and QRhi actually support. The GPU
  probe answers it before anything depends on it. `DeviceImage::format()` names
  a host layout precisely so that the answer has somewhere visible to land.
- **Fallback must not be able to hide a missing GPU.** A comparison through a
  renderer that silently falls back compares the CPU with itself. That contract
  belongs with the probe and the comparison tests, and is noted here so it is
  not lost between documents.
- **If QRhi proves unsuitable, nothing above changes.** Direct Vulkan implements
  the same boundary; `Qt6::GuiPrivate` and `QRhiTexture` appear in one
  translation unit, which is the whole point of putting them there.

## What was rejected, and why

**An adapter over a module that names nothing** — `Renderer` documented as the
CPU-output adapter, with retention private to whoever owns the device. It is the
smallest decision and it forecloses nothing, which is a real argument. It was
rejected because prefix reuse would then have no shared representation: the
preview would invent the rule for when a texture is still valid, in the app,
where the CLI and export cannot reuse it, and that invented rule would be a
second implementation of ADR 011's fold in a layer with no reason to be trusted
with it. Deferring here is deciding to have this argument again with code
already written against the gap.

**A separate opaque handle** — a third noun beside `ImageBuffer` and
`RenderCheckpoint`, with its own readback, presentation and lifetime rules. It is
the most direct about residency. It was rejected because the likely end state is
a handle that grows a boundary and a plan prefix, at which point it is a
checkpoint with a different name — and because these ADRs have repeatedly
refused to multiply mechanisms: two verbs deliberately in ADR 011, no callback
interface there, no generation counter in ADR 012.
