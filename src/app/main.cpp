#include "ui/MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    arraw::app::MainWindow window;
    window.show();

    return QApplication::exec();
}
