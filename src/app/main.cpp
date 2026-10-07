#include "DebugLog.h"
#include "QtMessageCapture.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QString>
#include <QTimer>

#include <filesystem>
#include <optional>

namespace {

/// @brief Sets the names and version the platform and Qt know the application by.
///
/// The application and organisation names decide where QSettings and
/// QStandardPaths keep their files (on Linux ~/.config/arraw/arraw.conf). The
/// domain is the macOS settings domain; the desktop file name lets a Wayland
/// compositor match the window to an installed .desktop entry, and so to its
/// icon.
void applyIdentity() {
    QApplication::setApplicationName(QStringLiteral("arraw"));
    QApplication::setOrganizationName(QStringLiteral("arraw"));
    QApplication::setOrganizationDomain(QStringLiteral("io.github.janpipek"));
    QApplication::setApplicationVersion(QStringLiteral(ARRAW_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("io.github.janpipek.arraw"));
}

/// @brief Collects the window icon from the PNGs compiled in from resources/icon.qrc.
///
/// The platform picks the best size for the title bar, taskbar or dock.
QIcon applicationIcon() {
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
        icon.addFile(QStringLiteral(":/icons/arraw-%1.png").arg(size));
    }
    return icon;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    // Messages before this go to the terminal only.
    arraw::app::DebugLog debugLog;
    const arraw::app::QtMessageCapture capture(debugLog.qtMessages);
    applyIdentity();
    // Before any widget exists, so the style and palette reach all of them.
    arraw::app::theme::apply(app);
    QApplication::setWindowIcon(applicationIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription("Open and develop photographs.");
    parser.addHelpOption();
    parser.addPositionalArgument(
        "path", "Photograph or folder to open; otherwise restore the last session.");
    parser.process(app);
    const auto arguments = parser.positionalArguments();
    if (arguments.size() > 1) {
        parser.showHelp(2);
    }
    std::optional<std::filesystem::path> path;
    if (!arguments.isEmpty()) {
        path = std::filesystem::path(arguments.front().toStdU16String());
    }

    arraw::app::MainWindow window(debugLog);
    window.show();
    QTimer::singleShot(0, &window, [&window, path] { window.openInitialPath(path); });

    return QApplication::exec();
}
