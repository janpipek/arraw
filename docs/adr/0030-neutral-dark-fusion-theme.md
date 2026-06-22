# Neutral dark photographer-friendly theme via Fusion + a single-sourced palette

Arraw ships a deliberate **neutral dark** UI: the Fusion style with a custom dark
`QPalette`, applied once at startup. All UI colors are single-sourced from a
dependency-free `src/ThemeColors.h`, anchored on the existing viewport surround
gray so chrome and the GPU canvas can never drift apart.

## Context

The app previously ran on the OS's native style with no theming — only one local
`setStyleSheet` (the semantic R/G/B/L curve buttons in `AdjustmentPanel`). A photo
editor wants the opposite of a colorful chromed UI:

- **Color neutrality.** Bright or tinted chrome around the image biases the eye's
  white-balance and contrast judgment. Every neutral must be strictly gray
  (R==G==B), and the image surround should stay the darkest large neutral on
  screen — the convention in Lightroom, Capture One, and darktable.
- **Cross-platform predictability.** Arraw is distributed as a Linux AppImage, a
  Windows installer, and a macOS build (ADR 0014). Native styles on Linux/macOS
  frequently *ignore* palette colors, and an AppImage can land on a host with no
  matching Qt platform theme — so a deliberate dark palette only survives on a
  style we control.
- **A single source of truth.** The surround gray already existed as a GPU clear
  color (`kClearColor` in `RendererCore`). The widget palette needs the same value
  to build panel grays relative to it; duplicating it would let the two drift.

## Design

- **`Theme::apply(QApplication&)`**, called from `main()` *before* `MainWindow` is
  constructed so the style/palette cascade reaches every widget, including native
  dialogs. Forces `QStyleFactory::create("Fusion")` unconditionally, then installs
  a dark `QPalette` built in one function (`buildDarkPalette`).
- **`src/ThemeColors.h`** — a leaf header of plain `QColor` constants with no
  widget dependencies. Both `RendererCore` (viewport surround) and `Theme` (the
  palette) read it. `ThemeColors::kCanvas` keeps the historical `0.15,0.15,0.15`
  value bit-identical so the golden-image references (ADR 0005) stay valid.
- **Panels slightly lighter than the canvas.** Window/docks `#2e2e2e`, raised
  panels `#36`, recessed `Base` `#1e1e1e`; the `#262626` canvas remains the
  darkest large neutral. A single muted steel-blue accent (`#3b6ea5`) for
  selection/focus only.
- **Palette first, zero QSS.** Fusion + the palette themes all standard widgets
  and any custom widget that already paints via `option.palette` (e.g. `FilmStrip`)
  for free. Only one chrome background was migrated to the shared constant
  (`Histogram`'s recessed background). Semantic/data-viz colors — histogram R/G/B
  channels, filmstrip flag/rating colors, curve channel buttons, viewport overlay
  and clipping warnings — stay hardcoded with their feature; they encode meaning,
  not chrome.

## Considered options

- **Keep the native style (rejected).** Feels at home, but native styles ignore
  the palette on Linux/macOS, so the dark theme would render half-applied and
  inconsistently across the shipped builds.
- **Light + dark toggle or follow-OS from day one (rejected, deferred).** A photo
  tool wants a controlled neutral environment regardless of the desktop. Dark-only
  is the norm and keeps one code path; the palette is built in one function so a
  light variant slots in behind the same seam later.
- **Heavy QSS restyle (rejected).** Per-widget QSS opts widgets out of Fusion's
  painting and tends to look worse and rot faster than a good palette. Any future
  QSS stays minimal and centralized in `Theme`, with the existing `AdjustmentPanel`
  sheet as the one sanctioned (semantic) exception.
- **User-settable colors now (deferred).** `ThemeColors.h` is structured so a
  future `QSettings` override is a one-function change; no settings key is wired
  yet to avoid speculative complexity.
- **Constant owned by `Theme`, or left in `RendererCore` (rejected).** Either makes
  the GPU renderer depend on the theme module or buries all app colors in a GPU
  file. The leaf `ThemeColors.h` avoids the layering inversion and is the natural
  home for the future settings load.

## Consequences

- `Theme::apply()` must stay before any widget construction in `main()`; moving it
  later would leave early widgets unthemed.
- `ThemeColors::kCanvas` must remain bit-identical to `0.15,0.15,0.15` — changing
  it drifts every golden image with surround pixels (ADR 0005). It is a *move*, not
  a *change*, of the value.
- Future readers should resist "theming" the semantic/data-viz colors; their fixed
  values are meaning, not styling.
- This is a UI presentation concern only — no new domain term, so `CONTEXT.md` is
  unchanged.
