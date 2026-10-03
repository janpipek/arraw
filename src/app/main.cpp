#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

#include <filesystem>
#include <optional>

int main(int argc, char* argv[]) {
    // Names where QSettings keeps what the application remembers.
    QCoreApplication::setOrganizationName("arraw");
    QCoreApplication::setApplicationName("arraw");
    QApplication app(argc, argv);

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

    arraw::app::MainWindow window;
    window.show();
    QTimer::singleShot(0, &window, [&window, path] { window.openInitialPath(path); });

    return QApplication::exec();
}
