#pragma once

#include <QCoreApplication>

// applicationName/organizationName feed QStandardPaths::AppDataLocation
// (docs/adr/0051): PresetStore::defaultPresetStore() and every Qt process
// that constructs an application object must resolve to the exact same
// directory, or the CLI silently reads/writes a different preset store than
// the Presets menu. Call this right after constructing any QCoreApplication
// (or subclass — QGuiApplication, QApplication) and before touching
// QStandardPaths. Single source of truth: GuiMain.cpp's QApplication and the
// CLI's QCoreApplication/QGuiApplication both call it instead of each
// hardcoding the two names.
inline void applyApplicationIdentity(QCoreApplication& app) {
    app.setApplicationName(QStringLiteral("arraw"));
    app.setOrganizationName(QStringLiteral("arraw"));
}
