#include "cli/Dispatch.h"
#include "core/AppIdentity.h"
#include <cstring>
#include <QCoreApplication>
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

    // Decided once, here at the edge, while stdout is still the process's own:
    // everything downstream just carries the answer (src/cli/TextStyle.h).
    const cli::TextStyle style = cli::TextStyle::forStdout();

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
        applyApplicationIdentity(app);
        return cli::dispatch(argc, argv, launchUi, out, err, style);
    }

    // preset reads/writes QStandardPaths::AppDataLocation (docs/adr/0051) via
    // PresetStore::defaultPresetStore(); that path depends on the identity
    // below, so it must exist before dispatch touches it. No display or
    // platform plugin is needed for pure sidecar I/O, so QCoreApplication is
    // enough (unlike export's QGuiApplication above).
    if (argc >= 2 && std::strcmp(argv[1], "preset") == 0) {
        QCoreApplication app(argc, argv);
        applyApplicationIdentity(app);
        return cli::dispatch(argc, argv, launchUi, out, err, style);
    }
    return cli::dispatch(argc, argv, launchUi, out, err, style);
}
