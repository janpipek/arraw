#include "MainWindow.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QSettings>
#include <QSurfaceFormat>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("arraw");
    app.setOrganizationName("arraw");

    // Neutral dark photographer-friendly theme (ADR 0030). Must run before any
    // widget is constructed so the Fusion style and palette cascade everywhere.
    Theme::apply(app);

    // Reverse-DNS identity shared across platforms (ADR 0014). organizationDomain
    // backs the macOS QSettings domain and the AppStream/desktop id; on Linux
    // QSettings still keys its path off organizationName, so leaving that as
    // "arraw" keeps ~/.config/arraw/arraw.conf exactly where it is. setDesktopFileName
    // lets a Wayland compositor match the running window to the installed .desktop
    // (so the taskbar shows our icon). Version is single-sourced from CMake.
    app.setOrganizationDomain("io.github.janpipek");
    app.setApplicationVersion(QStringLiteral(ARRAW_VERSION));
    app.setDesktopFileName(QStringLiteral("io.github.janpipek.arraw"));

    // Runtime window icon (title bar / taskbar / dock while running). The
    // platform picks the best size from the baked PNG set; native packaging
    // icons (.ico/.icns/.desktop) are a later step — see ADR 0013.
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/arraw-%1.png").arg(size));
    app.setWindowIcon(icon);

    // --version / --help exit early (handled by process()); a single optional
    // positional arg names a file or folder to open, replacing the old args[1].
    QCommandLineParser parser;
    parser.setApplicationDescription("arraw — a lightweight RAW photo editor");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("path", "Image file or folder to open.");
    parser.process(app);

    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    MainWindow w;
    w.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        w.openPath(args.first());
    } else {
        QSettings s;
        QString lastDir = s.value("lastDir").toString();
        if (!lastDir.isEmpty() && QDir(lastDir).exists()) {
            w.openPath(lastDir);
        }
    }

    return app.exec();
}
