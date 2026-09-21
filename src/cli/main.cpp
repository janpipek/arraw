#include "Cli.h"

#include <QCoreApplication>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

/// @brief Runs the command line inside a Qt application.
///
/// QCoreApplication rather than QGuiApplication: the engine needs no display,
/// but Qt resolves image-codec plugin paths against an application instance, so
/// without one every format beyond PNG would fail to encode.
///
/// Nothing but wiring lives here. ::arraw::cli::run takes its streams as
/// arguments so that the tests can drive it directly; see ADR 006.
int main(int argc, char* argv[]) {
    const QCoreApplication app(argc, argv);

    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    return arraw::cli::run(arguments, std::cout, std::cerr);
}
