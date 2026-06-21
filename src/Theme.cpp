#include "Theme.h"

#include "ThemeColors.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace Theme {

namespace {

// Builds the dark palette from ThemeColors. Kept as one function so the whole
// look lives in a single place (and a light variant / user overrides slot in
// here later).
QPalette buildDarkPalette() {
    using namespace ThemeColors;
    QPalette p;

    p.setColor(QPalette::Window, kWindow);
    p.setColor(QPalette::WindowText, kText);
    p.setColor(QPalette::Base, kBase);
    p.setColor(QPalette::AlternateBase, kAlternateBase);
    p.setColor(QPalette::Text, kText);
    p.setColor(QPalette::PlaceholderText, kPlaceholderText);

    p.setColor(QPalette::Button, kButton);
    p.setColor(QPalette::ButtonText, kText);
    p.setColor(QPalette::BrightText, kBrightText);

    // Bevels/borders Fusion derives from these roles.
    p.setColor(QPalette::Light, kBevelLight);
    p.setColor(QPalette::Midlight, kPanelRaised);
    p.setColor(QPalette::Mid, kWindow);
    p.setColor(QPalette::Dark, kBorder);
    p.setColor(QPalette::Shadow, kShadow);

    p.setColor(QPalette::Highlight, kHighlight);
    p.setColor(QPalette::HighlightedText, kHighlightedText);
    p.setColor(QPalette::Link, kLink);

    p.setColor(QPalette::ToolTipBase, kWindow);
    p.setColor(QPalette::ToolTipText, kText);

    // Disabled state: dim text/affordances so they read as inactive.
    p.setColor(QPalette::Disabled, QPalette::WindowText, kDisabledText);
    p.setColor(QPalette::Disabled, QPalette::Text, kDisabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, kDisabledText);
    p.setColor(QPalette::Disabled, QPalette::Highlight, kPanelRaised);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, kDisabledText);

    return p;
}

} // namespace

void apply(QApplication& app) {
    // Fusion is the only style that reliably paints from the palette across all
    // platforms and inside the AppImage (ADR 0030).
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setPalette(buildDarkPalette());
}

} // namespace Theme
