#pragma once

class QApplication;

namespace arraw::app::theme {

/// @brief Applies the neutral dark theme to the whole application.
///
/// Forces the Fusion style, the one style that honours a custom palette on
/// every platform, and installs a dark palette built from the theme colours. Call
/// once, before any widget is constructed, so the style and palette reach
/// every widget, dialogs included.
/// @param app Application to theme.
void apply(QApplication& app);

} // namespace arraw::app::theme
