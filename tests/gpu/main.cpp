#include "GpuTesting.h"
#include "HeadlessPlatform.h"

#include <QGuiApplication>
#include <QtGlobal>
#include <QtPlugin>

#include <catch2/catch_session.hpp>

// The platform the device is made on, linked in statically as arraw-cli links it.
Q_IMPORT_PLUGIN(HeadlessPlatformPlugin)

/// @brief Runs the GPU suite inside a QGuiApplication on arraw's own platform.
///
/// Makes the one shared device before Catch starts, so that a test asks
/// arraw::test::gpuContext() rather than paying for a device of its own. A
/// software rasteriser (lavapipe) is accepted: the passes' correctness is under
/// test, not the hardware. The device goes before the application, which its
/// platform instance belongs to.
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", arraw::headless::platformKey);
    const QGuiApplication app(argc, argv);
    arraw::test::createSharedGpuContext();
    const int result = Catch::Session().run(argc, argv);
    arraw::test::destroySharedGpuContext();
    return result;
}
