#pragma once
#include <QColor>

// Single source of truth for arraw's UI colors (ADR 0030). A dependency-free
// leaf header so both the GPU renderer (RendererCore, for the viewport surround)
// and the widget theme (Theme, which builds the QPalette) can read the same
// values without any layering inversion. This is also the one place a future
// user-settable theme would load overrides from QSettings.
//
// Design intent — a neutral, photographer-friendly dark UI:
//   * Every neutral is strictly gray (R==G==B) so nothing biases the eye's
//     white-balance/contrast judgment of the photo itself.
//   * Panels sit slightly LIGHTER than the viewport canvas, so the image
//     surround stays the darkest large neutral region on screen.
//   * One muted steel-blue accent for selection/focus only — never large fills.
namespace ThemeColors {

// The viewport surround / GPU clear color. Anchors the whole palette. This value
// is also baked into the golden-image test references (ADR 0005), so it MUST stay
// bit-identical to the historical 0.15 clear color — change it and the golden
// suite drifts.
inline const QColor kCanvas = QColor::fromRgbF(0.15f, 0.15f, 0.15f); // ~#262626

// Neutral chrome surfaces, built relative to the canvas.
inline const QColor kWindow = QColor(0x2e, 0x2e, 0x2e);      // docks, toolbars, window — slightly lighter than canvas
inline const QColor kPanelRaised = QColor(0x36, 0x36, 0x36); // group boxes / raised panels
inline const QColor kButton = QColor(0x3a, 0x3a, 0x3a);      // buttons — a touch of affordance lift
inline const QColor kBase = QColor(0x1e, 0x1e, 0x1e);        // recessed surfaces: text entry, histogram bg
inline const QColor kAlternateBase = QColor(0x262626);

// Borders / bevels (Fusion uses Mid/Dark/Light/Shadow for these).
inline const QColor kBorder = QColor(0x1a, 0x1a, 0x1a);
inline const QColor kBevelLight = QColor(0x44, 0x44, 0x44);
inline const QColor kShadow = QColor(0x14, 0x14, 0x14);

// Text.
inline const QColor kText = QColor(0xd6, 0xd6, 0xd6);
inline const QColor kBrightText = QColor(0xff, 0xff, 0xff);
inline const QColor kDisabledText = QColor(0x6a, 0x6a, 0x6a);
inline const QColor kPlaceholderText = QColor(0x80, 0x80, 0x80);

// The single accent: a muted, desaturated steel blue. Legible for filmstrip
// selection without flooding the screen with saturated color on multi-select.
inline const QColor kHighlight = QColor(0x3b, 0x6e, 0xa5);
inline const QColor kHighlightedText = QColor(0xf5, 0xf5, 0xf5);
inline const QColor kLink = QColor(0x5a, 0x9b, 0xd4);

} // namespace ThemeColors
