**GPU implementation idea — design review, 2026-09-22**

Reviewed [the proposal](../ideas/gpu-implementation-plan.md) against the current
processing, geometry, build and test code, ADRs 011–012, the reimplementation
plan, and Qt 6.10 documentation. This is a design review: findings identify
missing contracts, not defects in an existing GPU implementation.

The direction is sound: resolve settings once, retain a readable CPU reference,
serialize shader inputs explicitly, and prove the pointwise pass before adding
spatial work. A fullscreen fragment pass is sufficient for this milestone.
Keep QRhi as the first candidate. Four gaps should be addressed before treating
the proposal as implementation-ready.

1. **High — the proposed renderer interface cannot deliver the intended preview path.**
   Proposal lines 30–44 and 121–125.

   `render(ImageBuffer, ProcessingPlan) -> ImageBuffer` requires CPU pixels on
   every call. It is useful for the comparison milestone, but callers cannot
   request a retained GPU result or present it through this interface. Using it
   for slider changes would require readback and subsequent upload; bypassing
   it would require an additional interface that the plan has not described.

   Device ownership also constrains that second interface. QRhi resources belong
   to one instance, all use of an instance must stay on one thread, and resources
   are not directly shareable between instances. A standalone renderer's texture
   therefore cannot simply be handed to a separately initialized viewport.
   [Qt's threading contract](https://doc.qt.io/qt-6.10/qrhi.html#threading)

   Two viable solutions:

   - **Recommended for this milestone:** describe `Renderer` as the synchronous
     CPU-output adapter. Implement its GPU work in a reusable internal module
     that records passes using a supplied QRhi and retains textures. The offscreen
     adapter owns a device and reads back; preview integration supplies its
     presentation device and consumes the resulting texture on its owner thread.
   - Introduce an opaque internal rendered-image handle now, with explicit
     readback and presentation operations and documented device lifetime. This
     accommodates residency directly but adds ownership machinery earlier.

   Keep the GPU module reusable by the library and CLI; placing it exclusively
   under the application would make those callers depend on application code.
   Specify teardown and outstanding-readback ownership before implementation.

2. **High — texture precision is unresolved despite being part of pixel correctness.**
   Proposal lines 41–42, 95–104.

   The proposal mentions RGBA16F but accepts F32 input and compares against an
   F32 CPU result. Half-float storage changes samples before shader arithmetic:
   even identity processing can change alpha and RGB, small values can disappear,
   and large finite values can overflow. CPU development deliberately preserves
   negatives and values above white; pointwise alpha is copied unchanged in
   [Develop.cpp](../../src/core/Develop.cpp). A generic tolerance must not hide
   a different storage contract.

   **Recommendation:** start with RGBA32F upload, intermediate and readback
   textures on devices that support the required operations. Define absolute
   plus relative RGB tolerances, finite-result checks, and exact pointwise alpha
   preservation. Treat RGBA16F as a later measured precision/memory tradeoff with
   separate acceptance criteria. Alternatively, explicitly adopt half-float
   quantization now and compare against a reference that models quantization at
   every texture boundary, while separately measuring its error against F32.

   Upload bytes must match the chosen texture format; QRhi requires half-float
   source data for RGBA16F rather than converting F32 bytes automatically.
   [Qt upload contract](https://doc.qt.io/qt-6/qrhitexturesubresourceuploaddescription.html)

   Add negative channels, very dark values, fractional alpha, a non-symmetric
   colour matrix, and disabled shoulder (`shoulderKnee = infinity`) to the tests.
   Identity matrices alone cannot detect a transposed matrix upload.

3. **Medium — prefix reuse needs both source provenance and actual pass boundaries.**
   Proposal lines 121–125 and step 6.

   Today's [planFor()](../../src/core/ProcessingPlan.cpp) uses encoding,
   settings, dimensions and orientation, but does not identify the source
   pixels. Two different photographs with the same metadata and settings can
   have identical plans. Reusing textures on plan equality alone would display
   stale pixels when switching photos.

   Also, exposure, tone and the shoulder are fused into one pass. There is no
   texture after exposure to reuse when contrast changes. The existing
   [ADR 011](../adr/011-a-plan-spatial-passes-and-one-pointwise-chain.md)
   explicitly makes checkpoints pass boundaries. With the proposed two passes,
   a geometry-only edit can reuse the developed texture; a tone edit must rerun
   pointwise development and downstream geometry.

   **Recommendation:** defer result caching in the initial comparison renderer.
   At step 6, implement the provenance and pass-prefix rules already decided in
   [ADR 012](../adr/012-a-render-resolves-from-a-photo-and-a-request.md), or scope
   a preview cache to one owned immutable source and unconditionally discard it
   when that source changes. Merely keeping an `ImageBuffer` address is unsafe:
   buffers expose mutable samples and addresses can be reused. Distinguish
   allocation reuse from reuse of previously computed pixels.

4. **Medium — comparison tests need a mode that requires GPU execution.**
   Proposal lines 22 and 100–117.

   CPU fallback is desirable in normal use, but comparisons through a renderer
   with automatic fallback can compare the CPU against itself. Compiled shaders
   also do not prove that a device created the pipeline or executed it.

   **Recommendation:** tests call the GPU adapter directly, or use a mode that
   forbids fallback and reports the selected backend. Separate fallback tests
   from numerical comparisons. Require an executing GPU validation job for
   shader changes; developers without a suitable device may see an explicit
   skip, but the required job must fail on unavailability. Exercise each backend
   claimed as supported before releasing it. This restores the verification
   requirement already stated in the reimplementation plan, section 18.

   Define availability beyond successful device creation: required texture
   formats and usages, render-target/pipeline creation, texture dimensions,
   allocation failure, and float readback. In particular, QRhi does not guarantee
   floating-point texture readback on its OpenGL backend just because the texture
   format works. Define which failures permit CPU retry and report the reason.
   [Qt readback capabilities](https://doc.qt.io/qt-6.10/qrhi.html#Feature-enum)

Before coding, make these smaller contracts explicit:

- **Qt dependency:** CMake already selects Qt 6.10 or newer. QRhi requires
  `Qt6::GuiPrivate` and has limited compatibility guarantees; record the tested
  Qt minor versions and isolate that dependency in the GPU target. Direct Vulkan
  remains an alternative if a concrete QRhi limitation blocks the work, but does
  not need to be implemented alongside it.
  [Qt compatibility contract](https://doc.qt.io/qt-6.10/qrhi.html#details)
- **Geometry:** retain CPU output dimensions and pixel-centre conventions, edge
  clamping, and straight-alpha output. Bilinear filtering of straight RGB followed
  by premultiplication is incorrect: premultiply each neighbour before weighting,
  then unpremultiply the sum. The CPU also preserves the original sample for exact
  texel hits, including hidden RGB at zero alpha. A manual four-fetch shader is
  the clearest initial reference. Include asymmetric images, all source
  orientations, fractional crops and transparent coloured edges; account for
  framebuffer orientation differences between backends.
- **Milestone scope:** until GPU geometry exists, compare only the pointwise
  result, or explicitly run CPU geometry after GPU readback. Do not silently
  ignore a resolved geometry plan in something advertised as a complete render.
- **Measurements:** record upload, pass and readback times separately, then
  measure slider-to-display latency without readback during preview integration.
  Track peak memory: one 6000 × 4000 RGBA32F texture is 384 MB; three such textures
  require 1.152 GB before CPU buffers, staging or caches. Full-frame fallback is
  a reasonable first policy; tiling can follow demonstrated need.

Suggested order: settle the synchronous adapter and device ownership; choose
F32 storage and required capabilities; prove a tiny float upload/render/readback
with fallback disabled; add serialization and pointwise comparisons; add geometry;
then integrate presentation and source-aware caching. This preserves the proposal's
small first milestone without postponing decisions that determine its interface.

Validation was source/document inspection and checking official Qt documentation.
No GPU experiment or test suite was run. No production code or proposal text was
changed.
