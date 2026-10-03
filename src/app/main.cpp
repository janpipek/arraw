#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QIcon>
#include <QString>

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
    applyIdentity();
    // Before any widget exists, so the style and palette reach all of them.
    arraw::app::theme::apply(app);
    QApplication::setWindowIcon(applicationIcon());

    arraw::app::MainWindow window;
    window.show();

    return QApplication::exec();
}
