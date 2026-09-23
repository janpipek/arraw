#include <HeadlessPlatform.h>

#include <QGuiApplication>
#include <QtGlobal>
#include <QtPlugin>

#include <catch2/catch_session.hpp>

// The platform under test, linked in statically as arraw-cli links it.
Q_IMPORT_PLUGIN(HeadlessPlatformPlugin)

/// @brief Runs the headless suite inside a QGuiApplication on arraw's own platform.
///
/// Its own executable because the main suite's QCoreApplication is the point
/// of several of its tests, and a process has one application. The platform is
/// forced rather than defaulted: whatever QT_QPA_PLATFORM the runner sets (CI
/// sets offscreen), these tests are about arraw-headless.
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", arraw::headless::platformKey);
    const QGuiApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
