#pragma once
#include <functional>
#include <QString>
#include <QTextStream>

namespace cli {

// Launches the GUI with an optional path (empty = restore last dir).
// Injected so the front-end decides: in-process on Linux/macOS, a detached
// spawn of arraw-gui.exe on Windows — and tests substitute a recorder.
using GuiLauncher = std::function<int(const QString& openPath)>;

// The dispatch contract (docs/adr/0049): no arguments -> ui (the AppImage
// double-click path); a known command -> run it; anything else -> error
// with a suggestion, exit 2 — never a silent GUI launch. The export branch
// requires a QGuiApplication to already exist (the mains create it).
int dispatch(int argc, char** argv, const GuiLauncher& launchUi, QTextStream& out, QTextStream& err);

} // namespace cli
