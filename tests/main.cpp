#include <QCoreApplication>

#include <catch2/catch_session.hpp>

/// @brief Runs the suite inside a Qt application.
///
/// Catch2 supplies its own `main`, but the image codecs beyond PNG are Qt
/// plugins, and plugin paths are resolved against an application instance.
int main(int argc, char* argv[]) {
    const QCoreApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
