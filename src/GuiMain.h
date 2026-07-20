#pragma once
#include <optional>
#include <QString>

// The GUI application entry — main.cpp's former body (docs/adr/0049). When
// `openPath` is set, positional-argument parsing is skipped and the given
// path is opened (empty = last-dir restore); the Windows arraw-gui.exe
// passes nullopt so file associations' bare-path argv keeps working.
int runGuiMain(int& argc, char** argv, std::optional<QString> openPath = std::nullopt);
