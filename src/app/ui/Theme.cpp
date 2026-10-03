#include "Theme.h"

#include "ThemeColors.h"

#include <QApplication>
#include <QPalette>
#include <QString>
#include <QStyleFactory>

namespace arraw::app::theme {

namespace {

/// @brief Builds the dark palette from the theme colours.
QPalette darkPalette() {
    QPalette palette;

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternateBase);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::PlaceholderText, placeholderText);

    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, brightText);

    // Fusion derives its bevels and borders from these.
    palette.setColor(QPalette::Light, bevelLight);
    palette.setColor(QPalette::Midlight, panelRaised);
    palette.setColor(QPalette::Mid, window);
    palette.setColor(QPalette::Dark, border);
    palette.setColor(QPalette::Shadow, shadow);

    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, highlightedText);
    palette.setColor(QPalette::Link, link);

    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);

    // Dimmed, so disabled controls read as inactive.
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, panelRaised);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);

    return palette;
}

} // namespace

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setPalette(darkPalette());

    // Style sheets only for what the palette cannot express. Fusion frames a
    // toolbar with a bright line and draws its separators as a bright bevel;
    // flatten the one and dim the other. A toolbar given a style sheet drops
    // its palette background, hence restating it.
    app.setStyleSheet(QStringLiteral("QToolBar { border: none; background: %1; } "
                                     "QToolBar::separator { background: %2; width: 1px; "
                                     "height: 1px; margin: 5px 6px; }")
                          .arg(window.name(), separator.name()));
}

} // namespace arraw::app::theme
