#include "ConfigDirectoryRules.h"
#include "support/TempDir.h"

#include <ConfigDirectory.h>

#include <QStandardPaths>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

using namespace arraw;

namespace {

/// @brief Sets or removes ARRAW_CONFIG_DIR for a scope, restoring what it was.
class EnvironmentGuard {
public:
    EnvironmentGuard() : was_(qEnvironmentVariable(configDirectoryVariable)) {
        had_ = qEnvironmentVariableIsSet(configDirectoryVariable);
    }
    ~EnvironmentGuard() {
        if (had_) {
            qputenv(configDirectoryVariable, was_.toUtf8());
        } else {
            qunsetenv(configDirectoryVariable);
        }
    }
    EnvironmentGuard(const EnvironmentGuard&) = delete;
    EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;

private:
    QString was_;
    bool had_ = false;
};

} // namespace

TEST_CASE("ARRAW_CONFIG_DIR wins, and is made absolute", "[config]") {
    CHECK(resolveConfigDirectory(u"/srv/arraw-config", u"/home/a/.config") ==
          std::filesystem::path("/srv/arraw-config"));
    const auto relative = resolveConfigDirectory(u"config/here", u"/home/a/.config");
    CHECK(relative.is_absolute());
    CHECK(relative == (std::filesystem::current_path() / "config/here").lexically_normal());
}

TEST_CASE("An empty ARRAW_CONFIG_DIR is ignored", "[config]") {
    EnvironmentGuard guard;
    qputenv(configDirectoryVariable, QByteArray());
    CHECK(configDirectory() == std::filesystem::path(QStandardPaths::writableLocation(
                                                         QStandardPaths::GenericConfigLocation)
                                                         .toStdU16String()) /
                                   "arraw");
}

TEST_CASE("The generic configuration location is used, with arraw below it", "[config]") {
    CHECK(resolveConfigDirectory(u"", u"/home/a/.config") ==
          std::filesystem::path("/home/a/.config/arraw"));
    // The platform's rule is Qt's: the real answer ends in arraw whatever the platform.
    EnvironmentGuard guard;
    qunsetenv(configDirectoryVariable);
    CHECK(configDirectory().filename() == "arraw");
}

TEST_CASE("The variable overrides the real lookup", "[config]") {
    const test::TempDir dir;
    EnvironmentGuard guard;
    qputenv(configDirectoryVariable, QString::fromStdU16String(dir.path().u16string()).toUtf8());
    CHECK(configDirectory() == dir.path());
}

TEST_CASE("No location is an error naming what is missing", "[config]") {
    try {
        (void)resolveConfigDirectory(u"", u"");
        FAIL("expected an exception");
    } catch (const std::runtime_error& error) {
        const std::string what = error.what();
        CHECK(what.find("configuration") != std::string::npos);
        CHECK(what.find(configDirectoryVariable) != std::string::npos);
    }
}

TEST_CASE("The suite runs with a configuration directory of its own", "[config]") {
    REQUIRE(qEnvironmentVariableIsSet(configDirectoryVariable));
    const auto directory = configDirectory();
    const auto home = std::filesystem::path(QString(qgetenv("HOME")).toStdU16String());
    if (!home.empty()) {
        CHECK(directory.lexically_relative(home).string().rfind("..", 0) == 0);
    }
}
