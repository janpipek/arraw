#include "TestApp.h"
#include "cli/SystemInfoCommand.h"
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTextStream>

// ARRAW_RHI_BACKEND=null makes the GPU facts deterministic (matches
// test_HeadlessRenderContext.cpp) instead of depending on this machine's real
// driver: this exercises the same runSystemInfo() a real invocation takes,
// just with a backend that always exists.
namespace {
struct NullBackendGuard {
    NullBackendGuard() { qputenv("ARRAW_RHI_BACKEND", "null"); }

    ~NullBackendGuard() { qunsetenv("ARRAW_RHI_BACKEND"); }
};
} // namespace

TEST_CASE("system-info table lists Rendering, File Locations, and System sections") {
    testApp(); // platform plugin must exist before GL context creation
    NullBackendGuard guard;
    QString outText, errText;
    QTextStream out(&outText), err(&errText);

    REQUIRE(cli::runSystemInfo(false, out, err) == 0);
    CHECK(outText.contains("Backend"));
    CHECK(outText.contains("Software (Null)"));
    CHECK(outText.contains("Settings"));
    CHECK(outText.contains("App version"));
    CHECK(errText.isEmpty());
}

TEST_CASE("system-info --json emits one object with the same facts") {
    testApp();
    NullBackendGuard guard;
    QString outText, errText;
    QTextStream out(&outText), err(&errText);

    REQUIRE(cli::runSystemInfo(true, out, err) == 0);

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(outText.trimmed().toUtf8(), &parseError);
    REQUIRE(parseError.error == QJsonParseError::NoError);
    REQUIRE(doc.isObject());

    const QJsonObject root = doc.object();
    CHECK(root["gpuBackend"].toString() == "Software (Null)");
    CHECK(root["settingsPath"].toString().contains("arraw"));
    CHECK_FALSE(root["appVersion"].toString().isEmpty());
}
