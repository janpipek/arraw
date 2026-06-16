# App icon: one SVG is the source, runtime window icon now, native packaging later

The app gets a window icon — a film sprocket-shaft arrow on a charcoal tile,
mastered as a single committed **SVG**. The SVG is the one source of truth; a
small set of pre-rendered PNGs (16–256 px) baked into a Qt resource feeds the
runtime `QIcon` set via `QApplication::setWindowIcon`. We deliberately scope this
to the *runtime* icon only — the title-bar / taskbar / dock glyph shown while the
app runs — and defer the OS-level native icons (Windows `.ico` in the `.exe`,
macOS `.icns` in an `.app` bundle, Linux `.desktop` + `hicolor` PNGs) to a later
step, because those require packaging machinery this repo does not yet have.

The governing constraint is the same one that governs the rest of the codebase:
**the design exists exactly once** ([[spot-for-algorithms]]). The SVG is that one
copy; the PNGs are derived artifacts, checked in the same way the test DNG fixture
is generated and committed rather than rebuilt on every machine. `tools/render_icons.py`
regenerates them (`uv run`, `cairosvg` via PEP 723 inline metadata — no install
step, no build dependency).

## Considered Options

- **Render the SVG at runtime via `Qt6::Svg`.** The truest single-source story:
  ship only the SVG, no derived PNGs. Rejected for now — it adds the `Qt6::Svg`
  dependency plus the `qsvgicon` deploy plugin to a deliberately lean dependency
  list, and hands small-size rasterisation to Qt, where the two rows of sprocket
  holes blur uncontrolled at 16 px. Hand-tuned PNGs let us keep the glyph legible
  at title-bar size. The SVG remains the source either way; this only changes how
  it is consumed.
- **Build-time SVG → PNG in CMake.** Nothing checked in, no runtime dependency —
  but it makes a rasteriser (librsvg / Inkscape / ImageMagick) a *hard build
  dependency* on all three platforms, which fights the same lean-dependency goal.
  A regen script that runs only when the SVG changes keeps the tool out of the
  build entirely.
- **Do the native icons now too.** Rejected as scope. There is no `.app` bundle,
  no Windows `.rc`, and no install/`.desktop` story today; the icon glyph and the
  packaging that carries it are separable, and the self-contained tile design was
  chosen so the *same* SVG drops into that work unchanged when it happens.

## Consequences

- A future reader will find an SVG, a folder of committed PNGs, and a regen script
  for an icon that is **not wired into any installer or bundle** — that is
  intentional, recorded here, and the documented next step.
- The PNGs are generated; do not hand-edit them. Edit `resources/icon.svg` and
  rerun `uv run tools/render_icons.py`.
- The tile design (rounded charcoal square, off-white film shaft, teal chevron)
  is the macOS/Windows native-icon form already, so the native step is asset
  generation and packaging wiring, not a redesign.
- This is branding, not domain language: `CONTEXT.md` is deliberately left
  untouched.
