#include "Cli.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QtGlobal>

#if defined(ARRAW_HEADLESS_PLATFORM)
#include <HeadlessPlatform.h>

#include <QtPlugin>
#endif

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if defined(ARRAW_HEADLESS_PLATFORM)
// arraw's own platform, linked in statically; see src/platform/headless. At
// global scope, where the plugin's entry point is declared.
Q_IMPORT_PLUGIN(HeadlessPlatformPlugin)
#endif

namespace {

/// @brief Collects the arguments after the program name.
std::vector<std::string> argumentsOf(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return arguments;
}

/// @brief Prepares the platform a `QGuiApplication` will load, for a command-line tool.
///
/// On Linux, selects arraw's headless platform, with or without a display.
/// It needs no X or Wayland server, so the command line runs the same on a
/// desktop, a build machine and over SSH, and it is the platform that can make
/// a Vulkan instance without one, which Qt's offscreen platform cannot. It has
/// no OpenGL. An explicit `QT_QPA_PLATFORM` (or `-platform`) always wins, which
/// is how to reach OpenGL: `QT_QPA_PLATFORM=xcb` or `wayland`. Windows and macOS
/// need no display for a device, and keep Qt's own platform.
///
/// On macOS, keeps the process a background one. Qt otherwise turns a plain
/// executable into a foreground application, with a Dock icon and a claim on
/// focus for as long as the probe runs; a value already set is left alone.
void prepareGraphicsPlatform() {
#if defined(ARRAW_HEADLESS_PLATFORM)
    // Empty counts as unset, as it does for Qt: it names no platform.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", arraw::headless::platformKey);
    }
#elif defined(Q_OS_MACOS)
    if (!qEnvironmentVariableIsSet("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM")) {
        qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
    }
#endif
}

} // namespace

/// @brief Runs the command line inside a Qt application.
///
/// A QGuiApplication, because Vulkan instances and OpenGL surfaces come from
/// the platform plugin only it loads, and every command gets the same one
/// rather than guessing which will need a device. ARRAW_DISABLE_GPU is the way
/// out: with it, a QCoreApplication, so that no platform plugin is loaded and
/// no graphics driver touched; `gpu-test` then fails and says why. Either way
/// there is an application instance, which Qt resolves image-codec plugin paths
/// against: without one every format beyond PNG would fail to encode.
///
/// Nothing but wiring lives here. ::arraw::cli::run takes its streams as
/// arguments so that the tests can drive it directly; see ADR 006.
int main(int argc, char* argv[]) {
    std::unique_ptr<QCoreApplication> app;
    if (arraw::cli::gpuDisabled()) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        prepareGraphicsPlatform();
        app = std::make_unique<QGuiApplication>(argc, argv);
    }

    // Collected after the application: QGuiApplication consumes the options
    // that are its own, such as -platform, and they are not the command line's
    // to parse.
    return arraw::cli::run(argumentsOf(argc, argv), std::cout, std::cerr);
}
