#include "cli/Dispatch.h"
#include <cstring>
#include <QGuiApplication>
#include <QTextStream>

#if defined(Q_OS_WIN)
#include <QFileInfo>
#include <QProcess>
#include <windows.h>
#else
#include "GuiMain.h"
#endif

int main(int argc, char* argv[]) {
    QTextStream out(stdout);
    QTextStream err(stderr);

#if defined(Q_OS_WIN)
    // The GUI lives in its own GUI-subsystem executable beside this one
    // (docs/adr/0049): spawn it detached so the console prompt returns.
    const cli::GuiLauncher launchUi = [&err](const QString& openPath) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const QString gui
            = QFileInfo(QString::fromWCharArray(self)).absolutePath() + "/arraw-gui.exe";
        QStringList args;
        if (!openPath.isEmpty())
            args << openPath;
        if (!QProcess::startDetached(gui, args)) {
            err << "arraw: failed to launch " << gui << "\n";
            return 2;
        }
        return 0;
    };
#else
    const cli::GuiLauncher launchUi = [&](const QString& openPath) {
        return runGuiMain(argc, argv, openPath);
    };
#endif

    // export renders on a QRhi, which needs a QGuiApplication and a platform
    // plugin (QT_QPA_PLATFORM=offscreen on display-less machines, ADR 0022).
    if (argc >= 2 && std::strcmp(argv[1], "export") == 0) {
        QGuiApplication app(argc, argv);
        return cli::dispatch(argc, argv, launchUi, out, err);
    }
    return cli::dispatch(argc, argv, launchUi, out, err);
}
