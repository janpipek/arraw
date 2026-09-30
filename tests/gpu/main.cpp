#include "GpuTesting.h"

#include <QGuiApplication>
#include <QtGlobal>
#include <QtPlugin>

#include <catch2/catch_session.hpp>

#include <exception>
#include <iostream>

#if defined(ARRAW_HEADLESS_PLATFORM)
#include "HeadlessPlatform.h"

// The platform devices are made on where there is one, linked in statically as
// arraw-cli links it.
Q_IMPORT_PLUGIN(HeadlessPlatformPlugin)
#endif

/// @brief Runs the GPU suite inside a QGuiApplication, on the backend ARRAW_TEST_GPU_BACKEND names.
///
/// Makes the one shared device before Catch starts, so that a test asks
/// arraw::test::gpuContext() rather than paying for a device of its own. A
/// software rasteriser (lavapipe) is accepted: the passes' correctness is under
/// test, not the hardware. The device goes before the application, which its
/// platform instance belongs to.
///
/// The platform follows the backend. Where arraw has a headless platform
/// (Linux) every backend but OpenGL runs on it, whatever QT_QPA_PLATFORM says,
/// as it needs no display; OpenGL needs a platform with a GL context, so it
/// honours QT_QPA_PLATFORM (xcb under Xvfb, say). Windows and macOS use Qt's
/// native platform. A misspelt backend name fails the run with exit code 1
/// instead of skipping every test.
int main(int argc, char* argv[]) {
    try {
        [[maybe_unused]] const arraw::GpuBackend backend = arraw::test::gpuTestBackend();
#if defined(ARRAW_HEADLESS_PLATFORM)
        if (backend != arraw::GpuBackend::OpenGL) {
            qputenv("QT_QPA_PLATFORM", arraw::headless::platformKey);
        }
#endif
    } catch (const std::exception& problem) {
        std::cerr << problem.what() << '\n';
        return 1;
    }
    const QGuiApplication app(argc, argv);
    arraw::test::createSharedGpuContext();
    const int result = Catch::Session().run(argc, argv);
    arraw::test::destroySharedGpuContext();
    return result;
}
