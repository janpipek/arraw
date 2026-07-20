#include "cli/ExportCommand.h"
#include "render/HeadlessRenderContext.h"
#include "TestApp.h"
#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>

namespace {
// A 16×16 gradient PNG on disk — decodeImage's standard-image path picks it
// up, so the e2e test needs no RAW fixture.
QString writeTestPng(const QString& dir) {
    QImage img(16, 16, QImage::Format_RGB888);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            img.setPixelColor(x, y, QColor(x * 16, y * 16, 128));
    const QString path = QDir(dir).filePath("scene.png");
    REQUIRE(img.save(path));
    return path;
}
} // namespace

TEST_CASE("arraw export renders a file end to end", "[golden]") {
    testApp();
    {
        QString error;
        if (!HeadlessRenderContext::create(&error))
            SKIP("no headless GPU backend: " + error.toStdString());
    }

    QTemporaryDir tmp;
    const QString input = writeTestPng(tmp.path());
    const QString outDir = QDir(tmp.path()).filePath("out");

    cli::ExportInvocation inv;
    inv.inputs = {input};
    inv.outDir = outDir;

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    const int code = cli::runExport(inv, out, err);

    INFO(errText.toStdString());
    REQUIRE(code == 0);
    const QString produced = QDir(outDir).filePath("scene.jpg");
    REQUIRE(QFile::exists(produced));
    const QImage result(produced);
    REQUIRE(result.width() == 16);
    REQUIRE(result.height() == 16);
    REQUIRE(outText.contains("scene.jpg"));
    REQUIRE(outText.contains("1 exported, 0 failed"));
}

TEST_CASE("a missing input fails that file but exits 1, not 2", "[golden]") {
    testApp();
    {
        QString error;
        if (!HeadlessRenderContext::create(&error))
            SKIP("no headless GPU backend: " + error.toStdString());
    }

    QTemporaryDir tmp;
    const QString good = writeTestPng(tmp.path());
    cli::ExportInvocation inv;
    inv.inputs = {QDir(tmp.path()).filePath("missing.arw"), good};
    inv.outDir = QDir(tmp.path()).filePath("out");

    QString outText, errText;
    QTextStream out(&outText), err(&errText);
    const int code = cli::runExport(inv, out, err);

    REQUIRE(code == 1); // continue-on-error: the good file still exported
    REQUIRE(QFile::exists(QDir(inv.outDir).filePath("scene.jpg")));
    REQUIRE(errText.contains("missing.arw"));
    REQUIRE(outText.contains("1 exported, 1 failed"));
}
