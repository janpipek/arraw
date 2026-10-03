#include "ui/MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    // Names where QSettings keeps what the application remembers.
    QCoreApplication::setOrganizationName("arraw");
    QCoreApplication::setApplicationName("arraw");
    QApplication app(argc, argv);

    arraw::app::MainWindow window;
    window.show();

    return QApplication::exec();
}
