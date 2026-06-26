// StandardImageLoader decodes non-RAW images via QImage. JPEG decoding depends on
// Qt's qjpeg imageformats plugin being deployed next to the binary; without it
// QImage returns null and load() reports "Failed to load" — the symptom users hit
// for every JPG on Windows. PNG is built into Qt Gui, so it never surfaced this.

#include "StandardImageLoader.h"
#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>

namespace {

// QImage plugin loading + the QGuiApplication library paths need an app instance.
// Mirrors the lazy-static idiom in the widget tests; each ctest runs isolated.
void ensureApp() {
    static int argc = 1;
    static char arg0[] = "arraw_tests";
    static char* argv[] = {arg0, nullptr};
    if (!qApp)
        new QApplication(argc, argv);
    if (!qobject_cast<QApplication*>(qApp))
        SKIP("needs a QApplication; run isolated (ctest does this per-test)");
}

} // namespace

TEST_CASE("StandardImageLoader decodes a JPEG without error", "[loader]") {
    ensureApp();

    // Round-trip through a real .jpg file; save also exercises the qjpeg plugin.
    QImage src(16, 16, QImage::Format_RGB32);
    src.fill(Qt::green);
    const QString path = QDir::temp().filePath("arraw_loader_test.jpg");
    REQUIRE(src.save(path, "JPEG"));

    const LoadResult result = StandardImageLoader::load(path);

    CHECK(result.error.isEmpty());
    CHECK(result.fullRes.valid());
    CHECK_FALSE(result.sensorClipFullRes.valid());
    CHECK_FALSE(result.sensorClipPreview.valid());

    QFile::remove(path);
}
