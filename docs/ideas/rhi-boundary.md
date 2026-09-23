# The GPU renderer boundary — options for review

Status: options paper, for review. Nothing here is decided. The outcome should
become an ADR; this document exists so the trade-offs are argued once, in the
open, before that ADR fixes one of them.

Context: the [GPU implementation plan](../ideas/gpu-implementation-plan.md)
proposes `Renderer::render(ImageBuffer, ProcessingPlan) -> ImageBuffer` as the
backend boundary. Its [review](../reviews/gpt-6_2026-09-22_gpu-implementation.md)
rejects that interface as unable to deliver the intended preview path, and
offers two replacements. A third is possible. This paper sets out all three.

No GPU code exists yet: `Renderer` and QRhi appear nowhere in `src/` or
`include/`. Nothing below is a migration; all of it is a first choice.

## Why the interface, and not the shader, is the hard part

`render(ImageBuffer) -> ImageBuffer` forces CPU pixels at both ends of every
call. For the comparison milestone that is exactly right — the tests want an
`ImageBuffer` to compare. For a preview it is wrong in a way that cannot be
patched: moving a slider would read a full-resolution texture back to the host
and upload it again, and a viewport that wanted to present the texture directly
would have to reach around the interface through something the plan does not
describe.

That second, undescribed path is the whole difficulty, because it is not free
to invent later. It has to say who owns the graphics device, what a retained
result *is*, and how a stale one is prevented — and those answers reach into
`ProcessingPlan`, into ADR 011's checkpoints, and into the CMake target graph.

## Constraints that bind

**QRhi device affinity.** Resources belong to one `QRhi` instance, all use of an
instance must stay on one thread, and resources are not shareable between
instances. A preview viewport and an offscreen exporter are therefore two
devices, and a texture made by one is useless to the other. (Source: the review's
citation of [Qt's threading contract](https://doc.qt.io/qt-6.10/qrhi.html#threading).
Not independently verified here — no network access in this environment, and
Qt is not installed, so this should be confirmed against the docs for the Qt
version we pin.)

**The reimplementation plan already requires residency.** §12, "GPU ownership and
presentation": *"Give each GPU context a defined owner thread… Keep pixels on the
GPU across compatible passes and, where possible, through viewport presentation.
Read back for export or requested CPU arrays. Avoid designs that transfer
full-resolution pixels between CPU and GPU on every stage."* The proposed
interface contradicts a requirement we already wrote down; this is not a new
concern raised by the review.

**ADR 011 already names the retained intermediate.** A `RenderCheckpoint` is
pixels, the boundary they were taken at, and the part of the plan that made them,
produced by `stopAfter` and consumed by `resumeFrom`, and validated by comparing
plan prefixes with the `stagesOf` fold. It is *decided but unbuilt* — ADR 011
says it is "decided here and built with the processor that caches them". So this
choice either uses that noun or deliberately stands a second one beside it.

**ADR 001 keeps `ImageBuffer` a plain raster.** Provenance lives on the
checkpoint, not on the buffer, explicitly so that no buffer becomes the
partially populated state ADR 001 forbids. Whatever we choose must not start
attaching device state to `ImageBuffer`.

**Core has no Qt in its headers.** `arraw` links `Qt6::Gui` PRIVATE and sets
`AUTOMOC OFF` because "the engine has no Qt types"
([CMakeLists.txt:71-74](../../CMakeLists.txt)). QRhi needs `Qt6::GuiPrivate`,
whose compatibility guarantees are weaker than public Qt's.

## What is *not* in dispute

Two points are worth separating out, because framing them as open makes the real
decision look larger than it is.

**Device ownership is effectively settled.** All three options below arrive at
the same shape: pass-recording code takes a device it does not own, and a
separate offscreen adapter owns one. Anything else fails immediately — code that
owned its device could never render into the viewport's, and code that could
only use a supplied device could not serve the CLI, which has no viewport.

**The CMake split follows from the existing discipline.** A separate `arraw-gpu`
target links `Qt6::GuiPrivate`; `arraw` core keeps its no-Qt-in-public-headers
property; the CLI and the tests may link `arraw-gpu` without linking the app.
`ShaderTools` is already a required component
([CMakeLists.txt:17](../../CMakeLists.txt)), so `qt6_add_shaders` is available
today. Putting the GPU module under `src/app` — which the plan's wording invites
— would make the library and CLI depend on application code, and should not be
done.

So the live question is narrow: **what is the retained GPU result called, and
who else may name it?**

## Option A — an adapter, over a module that names nothing

`Renderer` is documented as the synchronous CPU-output adapter: it is what the
tests, the CLI and export use, and readback is its job, not an accident. Beneath
it, an internal pass-recording module takes a supplied `QRhi` and returns a
texture it does not own. Retention is private to whoever owns the device.

```
  library / CLI                app (preview)
       |                            |
  owns QRhi (offscreen)        owns QRhi (swapchain)
       |                            |
       v                            v
  +----------------+        +-----------------+
  |  GpuRenderer   |        | ViewportRender  |
  +----------------+        +-----------------+
         \                         /
          \___ supplies QRhi& ____/
                     |
                     v
          +---------------------------+
          |   GpuPasses  (internal)   |
          |  records passes, returns  |
          |  a texture it DOESN'T own |
          +---------------------------+
                     |
          readback   v
  Renderer::render(ImageBuffer, Plan) -> ImageBuffer
```

**For**

- Smallest decision that unblocks the first milestone. The comparison tests get
  exactly the interface they want, and nothing else is committed to.
- Adds no noun to the core vocabulary. ADR 011's checkpoint stays untouched and
  unbuilt, free to be designed against real caching experience later.
- Keeps the first GPU code honest about what it is: a thing that records passes.
  The residency question is deferred rather than guessed at.

**Against**

- Prefix reuse has no shared representation. When the preview wants to keep the
  developed texture across a geometry-only edit, it must invent the rule for
  when that texture is still valid — and the natural place to invent it is the
  app, where the CLI and export cannot reuse it.
- That invented rule will be a second implementation of ADR 011's plan-prefix
  fold, in a layer that has no reason to be trusted with it. The most likely
  failure is the one the review names: reuse keyed on something weaker than the
  plan, showing stale pixels.
- Defers rather than avoids. Every caller that wants residency will need a name
  for what it is holding; "internal, untyped" is a decision to have this argument
  again, later, with code already written against the gap.

## Option B — the checkpoint carries device pixels

ADR 011's `RenderCheckpoint` gains a backend-dependent payload: an `ImageBuffer`
on the CPU, an opaque device texture plus its owning device on the GPU. The
boundary and plan prefix are unchanged, so validation is the fold ADR 011
already specifies, for both backends.

```
  RenderCheckpoint {
      boundary                 <- ADR 011, unchanged
      plan prefix              <- ADR 011, unchanged
      pixels: ImageBuffer            (CPU)
            | DeviceTexture + device (GPU)   <- the only new thing
  }

  CPU backend --stopAfter--> Checkpoint{ImageBuffer}
  GPU backend --stopAfter--> Checkpoint{DeviceTexture}
                                    |
                       +------------+-----------+
                       |                        |
                  readBack()               present()
                       |                  (same device ONLY)
                       v                        v
                  ImageBuffer               viewport
```

**For**

- One retention vocabulary instead of two. `stopAfter` and `resumeFrom` mean the
  same thing on both backends, which is the divergence ADR 009 and ADR 011 exist
  to prevent.
- Prefix reuse is free and shared. The app, the CLI and export all get the same
  validity rule, already written, already argued, already guarded by
  `static_assert` on the stage tuple.
- Device affinity becomes a checkable field rather than a convention. "This
  texture belongs to the export device" is something code can refuse, instead of
  something a comment asks people to remember.
- Costs nothing to adopt now. Checkpoints are unbuilt, so there is no migration —
  only the question of whether we want to decide this before writing GPU code.

**Against**

- Puts a device-side noun in the core vocabulary, even as an opaque handle. Core
  currently knows nothing about graphics, and this is the first crack in that.
- Decides more, earlier, on the least evidence. We have not yet rendered a single
  pixel on the GPU; committing the checkpoint's shape now means committing it
  without having felt where it chafes.
- `RenderCheckpoint` becomes a variant, and its consumers grow a case they did
  not have. A caller that just wants pixels must now ask, or be handed an
  interface that hides a readback with a real cost behind an innocuous accessor.
- Risks over-fitting ADR 011's design to the GPU before the CPU processor that
  ADR 011 was actually written for exists.

## Option C — a separate opaque handle

A third noun, beside `ImageBuffer` and `RenderCheckpoint`, with explicit
readback and presentation operations and documented device lifetime.

```
  Renderer::render(...)    -> ImageBuffer
                              (tests, CLI, export)

  GpuRenderer::render(...) -> RenderedImage   <- new noun
                                   |
            +----------+-----------+--------+
            |          |           |        |
       readBack()  present()   device()  boundary()
            |          |
            v          v
      ImageBuffer   viewport

  meanwhile, still unbuilt and unreconciled:
      RenderCheckpoint { pixels, boundary, plan }
```

**For**

- Most direct about residency. The thing that lives on the device has a name, a
  lifetime and operations, and none of it is implied.
- Leaves ADR 011 entirely alone, so a later CPU processor is not constrained by
  a GPU-driven change to the checkpoint.
- The honest option if we think the GPU's retained result genuinely is a
  different kind of thing from a resumable checkpoint — for instance if it turns
  out that presentable textures and resumable intermediates have different
  lifetime rules in practice.

**Against**

- Two retention concepts, to be reconciled later or never. The likely end state
  is a `RenderedImage` that grows a plan prefix and a boundary, at which point it
  *is* a checkpoint with a different name.
- Most machinery, earliest, for the least immediate return. None of the first
  milestone needs it.
- A new noun in a codebase whose ADRs repeatedly argue against multiplying
  mechanisms — ADR 011's "two verbs, deliberately", its refusal of a callback
  interface, its refusal of a generation counter in ADR 012.

## Where the options actually differ

| | A: adapter | B: checkpoint | C: handle |
|---|---|---|---|
| Nouns added to core | none | one (opaque device texture) | one (and a second later) |
| Prefix-reuse rule | invented in the app | ADR 011's fold, unchanged | its own, new |
| CLI/export share reuse | no | yes | yes |
| Stale-texture failure | possible, app-local | structurally prevented | depends on new rules |
| Decided before first pixel | least | most | middling |
| Cost if wrong | argue again, with code written | unpick a variant | reconcile two nouns |

## A correction to the review

The review's finding 3 argues that a texture cache keyed on plan equality would
show stale pixels, because `planFor()` "uses encoding, settings, dimensions and
orientation, but does not identify the source pixels", so two photographs can
produce identical plans.

That is true of the narrow overload the GPU plan targets, `planFor(encoding,
settings)` ([ProcessingPlan.h:111](../../src/core/ProcessingPlan.h)). It is not
true of the design: ADR 012 puts the file's name and a stamp of its size and
modification time in the decode block, precisely so that "everything that decides
which pixels come out is therefore compared by value". Once plans resolve from a
`Photo`, the collision cannot occur.

So the finding is real but narrower than stated, and its remedy is not new
provenance machinery — it is that the GPU work must not build a cache against the
narrow overload. Under option B this is automatic. Under A and C it is a rule
someone has to remember.

## Recommendation

**Option B, with one hedge.** The argument that decides it is not the GPU's: it
is that ADR 011 already spent its length arguing that reuse must be validated by
comparing plan prefixes, and options A and C both create a place where that rule
will be re-implemented by someone in a hurry. Adopting the existing noun costs
one opaque type in core and buys the validity rule, the CLI's access to it, and
the review's finding 3 dissolving.

The hedge: the ADR should decide *that* the checkpoint carries a
backend-dependent payload, and leave the payload's operations to be built with
their first caller, as ADR 011 does for `sample`, `stopAfter` and the diagnostics
sink. That keeps the commitment to the part we can argue from existing decisions,
and keeps speculation out of it.

If the objection to B is that it decides too much too early, **option A is the
defensible fallback** — but it should then say explicitly, in the ADR, that
preview retention is undecided and that no cache may be built in the app without
returning to this question. An unstated deferral is how A turns into C by
accident.

## What this does not decide

- **Texture precision** (review finding 2). Whether storage is RGBA32F or RGBA16F
  is an empirical question about what the hardware and QRhi actually support,
  and the [GPU probe spike](../../tests) should answer it before an ADR commits.
- **Fallback policy and how tests forbid it** (review finding 4).
- **Geometry semantics** — pixel centres, edge clamping, premultiplication order,
  framebuffer orientation. The review's notes on these stand and are unaffected
  by this choice.
- **Whether QRhi survives contact with `Qt6::GuiPrivate` on our pinned Qt.** If
  it does not, the same boundary is implemented over Vulkan directly, and none
  of the above changes.
