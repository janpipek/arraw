#pragma once

#include <QColor>

/// @brief Colours of the desktop application's neutral dark theme.
///
/// The single source for every UI colour, so the widget palette and anything
/// that paints by hand (the image surround, a histogram) agree.
///
/// Design intent:
/// - Every neutral is strictly grey (R == G == B), so nothing biases the
///   eye's judgement of the photograph's white balance and contrast.
/// - Panels sit slightly lighter than the canvas, so the image surround stays
///   the darkest large neutral region on screen.
/// - One muted steel-blue accent, for selection and focus only, never for
///   large fills.
namespace arraw::app::theme {

/// Image surround; anchors the whole palette.
inline const QColor canvas{0x26, 0x26, 0x26};

/// Docks, toolbars and the window, slightly lighter than the canvas.
inline const QColor window{0x2e, 0x2e, 0x2e};
/// Group boxes and other raised panels.
inline const QColor panelRaised{0x36, 0x36, 0x36};
/// Buttons, lifted a touch above the window for affordance.
inline const QColor button{0x3a, 0x3a, 0x3a};
/// Recessed surfaces: text entry, list views, histogram background.
inline const QColor base{0x1e, 0x1e, 0x1e};
/// Alternating rows of recessed surfaces.
inline const QColor alternateBase{0x26, 0x26, 0x26};

/// Dark border, which Fusion draws from QPalette::Dark.
inline const QColor border{0x1a, 0x1a, 0x1a};
/// Light bevel edge, which Fusion draws from QPalette::Light.
inline const QColor bevelLight{0x44, 0x44, 0x44};
/// Shadow, which Fusion draws from QPalette::Shadow.
inline const QColor shadow{0x14, 0x14, 0x14};

/// Regular text.
inline const QColor text{0xd6, 0xd6, 0xd6};
/// Text that must stand out against any surface.
inline const QColor brightText{0xff, 0xff, 0xff};
/// Text of disabled controls.
inline const QColor disabledText{0x6a, 0x6a, 0x6a};
/// Placeholder text in empty entry fields.
inline const QColor placeholderText{0x80, 0x80, 0x80};
/// Text of warnings, such as a log's warning rows; muted, like the accent.
inline const QColor warningText{0xd6, 0xa6, 0x4a};
/// Text of errors, such as a log's error rows; muted, like the accent.
inline const QColor errorText{0xe0, 0x6c, 0x64};

/// Fill and outline of the render pie when the picture is up to date; muted green, about
/// 4.9:1 against the window grey.
inline const QColor progressDone{0x5a, 0xa8, 0x6a};
/// Fill and outline of the render pie while a render is going; a clear, more saturated red than
/// ::arraw::app::theme::errorText, so that a render does not read as an error, about 3.8:1
/// against the window grey.
inline const QColor progressBusy{0xf0, 0x44, 0x3c};
/// Outline of the render pie while a photograph is opening and no render is to be shown yet;
/// the done green dimmed, so a quick switch between photographs keeps the pie greenish rather
/// than blinking through grey, and it still reads as not yet done; about 2.3:1 against the window
/// grey.
inline const QColor progressOpening{0x3c, 0x6e, 0x47};
/// Outline of the render pie with no photograph open; neutral grey, about 3.5:1 against the
/// window grey.
inline const QColor progressIdle{0x80, 0x80, 0x80};

/// Toolbar separators: a dim divider only a touch off the window grey.
inline const QColor separator{0x3a, 0x3a, 0x3a};

/// The single accent, legible for selection without flooding the screen on
/// multi-select.
inline const QColor highlight{0x3b, 0x6e, 0xa5};
/// Text on the accent.
inline const QColor highlightedText{0xf5, 0xf5, 0xf5};
/// Hyperlinks.
inline const QColor link{0x5a, 0x9b, 0xd4};

} // namespace arraw::app::theme
