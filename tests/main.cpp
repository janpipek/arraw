#include <ConfigDirectory.h>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_session.hpp>

/// @brief Runs the suite inside a Qt application.
///
/// Catch2 supplies its own `main`, but the image codecs beyond PNG are Qt
/// plugins, and plugin paths are resolved against an application instance.
/// The configuration directory is a temporary one of this process, even when the caller set
/// one, so that no test reads or writes the user's own files.
int main(int argc, char* argv[]) {
    const QCoreApplication app(argc, argv);
    QTemporaryDir configuration;
    if (configuration.isValid()) {
        qputenv(arraw::configDirectoryVariable, configuration.path().toUtf8());
    }
    return Catch::Session().run(argc, argv);
}
