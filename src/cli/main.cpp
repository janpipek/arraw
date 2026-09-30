#include "Cli.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QtGlobal>

#if defined(ARRAW_HEADLESS_PLATFORM)
#include "HeadlessPlatform.h"

#include <QtPlugin>
#endif

#include <array>
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
/// no OpenGL. An explicit `QT_QPA_PLATFORM` wins, which is how to reach
/// OpenGL: `QT_QPA_PLATFORM=xcb` or `wayland`, except for a command that
/// asks for no display (`forced`). Windows and macOS
/// need no display for a device, and keep Qt's own platform.
///
/// On macOS, keeps the process a background one. Qt otherwise turns a plain
/// executable into a foreground application, with a Dock icon and a claim on
/// focus for as long as the probe runs; a value already set is left alone.
///
/// @param forced Whether to select the headless platform even over an explicit
/// `QT_QPA_PLATFORM`: for a command that needs no display, which should not
/// abort because the environment names a platform that cannot start.
void prepareGraphicsPlatform(bool forced) {
#if defined(ARRAW_HEADLESS_PLATFORM)
    // Empty counts as unset, as it does for Qt: it names no platform.
    if (forced || qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", arraw::headless::platformKey);
    }
#elif defined(Q_OS_MACOS)
    if (!qEnvironmentVariableIsSet("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM")) {
        qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
    }
#endif
}

} // namespace

/// @brief Runs the command line, inside the Qt application its command asks for.
///
/// None until then: a command asks once its arguments are good, so help and
/// usage errors load no platform plugin, which would abort the process if it
/// could not start. An export asks for a QCoreApplication, which Qt resolves
/// image-codec plugin paths against; the GPU probe for a QGuiApplication,
/// because Vulkan instances and OpenGL surfaces come from the platform plugin
/// only it loads.
///
/// Qt is shown the program name and nothing else. Given all of `argv` it would
/// remove the options it takes for its own, `-reverse` or `-platform` among
/// them, even after `--`, where they are input files; `QT_QPA_PLATFORM` is the
/// way to choose a platform.
///
/// Nothing but wiring lives here. ::arraw::cli::run takes its streams and the
/// application's construction as arguments so that the tests can drive it
/// directly; see ADR 006.
int main(int argc, char* argv[]) {
    const std::vector<std::string> arguments = argumentsOf(argc, argv);

    // Both must outlive the application, which keeps references to them.
    static char fallbackName[] = "arraw-cli";
    int qtArgc = 1;
    std::array<char*, 2> qtArgv{argc > 0 ? argv[0] : fallbackName, nullptr};

    std::unique_ptr<QCoreApplication> app;
    const auto start = [&](arraw::cli::ApplicationKind kind) {
        if (app) {
            return;
        }
        if (kind != arraw::cli::ApplicationKind::Core) {
            prepareGraphicsPlatform(kind == arraw::cli::ApplicationKind::OffscreenDevice);
            app = std::make_unique<QGuiApplication>(qtArgc, qtArgv.data());
        } else {
            app = std::make_unique<QCoreApplication>(qtArgc, qtArgv.data());
        }
    };
    return arraw::cli::run(arguments, std::cout, std::cerr, start);
}
