#include "MainWindow.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QSettings>
#include <QDir>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("arraw");
    app.setOrganizationName("arraw");

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
