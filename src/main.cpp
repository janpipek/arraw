#include "MainWindow.h"
#include <QApplication>
#include <QIcon>
#include <QSurfaceFormat>
#include <QSettings>
#include <QDir>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("arraw");
    app.setOrganizationName("arraw");

    // Runtime window icon (title bar / taskbar / dock while running). The
    // platform picks the best size from the baked PNG set; native packaging
    // icons (.ico/.icns/.desktop) are a later step — see ADR 0013.
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/arraw-%1.png").arg(size));
    app.setWindowIcon(icon);

    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    MainWindow w;
    w.show();

    const QStringList args = app.arguments();
    if (args.size() > 1) {
        w.openPath(args.at(1));
    } else {
        QSettings s;
        QString lastDir = s.value("lastDir").toString();
        if (!lastDir.isEmpty() && QDir(lastDir).exists()) {
            w.openPath(lastDir);
        }
    }

    return app.exec();
}
