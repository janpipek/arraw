# RHI migration: big-bang port, qsb-baked shaders, one renderer core

`ImageViewport` moves from `QOpenGLWidget` + raw GL 3.3 to `QRhiWidget`
(Qt floor → 6.8 LTS, `Qt6::GuiPrivate` for the semi-public RHI headers) so the
app renders natively on Metal/Vulkan/D3D11 — macOS's frozen OpenGL stack is the
practical driver. The port is a big-bang on one branch: the raw-GL code is
deleted in the same PR, with the ADR 0005 goldens as the merge gate (their
dual tolerance absorbs backend variance, so the goldens are *not*
regenerated — a systematic shift on a new backend is a real bug, not noise).

The governing constraint is a single point of truth for the algorithm:

- **One shader source.** `image.frag`/`image.vert` are rewritten once into
  Vulkan-dialect GLSL (`#version 440`, uniform block, explicit bindings) and
  compiled at build time by qsb (`qt6_add_shaders`) into Qt resources for all
  backends. No per-backend shader forks. Runtime loading from
  `build/shaders/` — and the never-wired `reloadShaders()` hot-reload — are
  deleted; shader iteration is rebuild + goldens.
- **One render pass recording.** A renderer core records the pass (pipeline,
  bindings, uniforms, draw) parameterized only by render target and
  resolution; widget paint, `renderToImage()` (signature unchanged — golden
  tests and MainWindow untouched), and the ADR 0004 histogram samples are
  three callers of that one path, instead of three FBO/draw dances sharing
  only a uniform-fill helper as under GL.

## Considered Options

- **Side-by-side GL + RHI viewports behind a flag**: safer rollback, but two
  texture-management and uniform-plumbing paths violate the SPOT constraint,
  and the goldens already provide the safety the flag would.
- **Port the three render paths as-is**: smaller diff to review against the GL
  code, but re-bakes the duplication into the new stack where it is harder to
  remove later.
- **Windowless renderer core (own `QRhi`, no widget)**: enables headless
  goldens and CLI batch export, but expands an already-risky PR; the renderer
  core keeps that door open.

## Consequences

- Backend is the Qt platform default: Metal on macOS, D3D11 on Windows,
  OpenGL-via-RHI on Linux (Vulkan opt-in through `QRhiWidget::setApi()`) — no
  GL code of our own either way.
- Export and histogram readbacks run in offscreen RHI frames
  (`beginOffscreenFrame`/`endOffscreenFrame`), which complete readbacks
  synchronously — `renderToImage()` keeps its blocking contract.
- Readback row order is backend-dependent: rows are flipped iff
  `QRhi::isYUpInFramebuffer()` (OpenGL).
- QRhiWidget cannot be over-painted with QPainter (QOpenGLWidget could), so
  the crop overlay / align grid moved to a transparent child widget.
- Shaders live in resources: the "check `build/shaders/` exists" failure mode
  is gone; docs referring to it must be updated.
- `Qt6::OpenGL`/`Qt6::OpenGLWidgets`/`find_package(OpenGL)` are dropped.
- RHI is source- but not binary-compatible across Qt minors; minor-version
  bumps may need small fixes.
