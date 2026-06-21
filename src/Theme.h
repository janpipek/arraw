#pragma once

class QApplication;

// Applies arraw's neutral dark photographer-friendly theme (ADR 0030): forces the
// Fusion style (the only style that reliably honors a custom palette across
// Fedora/Windows/macOS and inside the AppImage) and installs the dark QPalette
// built from ThemeColors. Call once from main() BEFORE constructing any widget so
// the style/palette cascade reaches everything, including native dialogs.
//
// Dark-only for now; the palette is built in one function so a light variant or a
// user-settable theme (from QSettings) can be added behind the same seam later.
namespace Theme {

void apply(QApplication& app);

} // namespace Theme
