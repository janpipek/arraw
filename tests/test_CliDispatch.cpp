#include "cli/Dispatch.h"
#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {
struct Harness {
    QString outText, errText;
    QTextStream out{&outText}, err{&errText};
    QStringList launches;
    cli::GuiLauncher launcher = [this](const QString& p) {
        launches << p;
        return 0;
    };
    int run(std::vector<const char*> argv) {
        argv.insert(argv.begin(), "arraw");
        return cli::dispatch(int(argv.size()), const_cast<char**>(argv.data()), launcher, out, err);
    }
};
} // namespace

TEST_CASE("bare invocation opens the UI (rule 1)") {
    Harness h;
    REQUIRE(h.run({}) == 0);
    REQUIRE(h.launches == QStringList{""});
}

TEST_CASE("ui command forwards its path") {
    Harness h;
    REQUIRE(h.run({"ui", "photo.arw"}) == 0);
    REQUIRE(h.launches == QStringList{"photo.arw"});
}

TEST_CASE("version prints and exits 0") {
    Harness h;
    REQUIRE(h.run({"version"}) == 0);
    REQUIRE(h.outText.startsWith("arraw "));
    REQUIRE(h.launches.isEmpty());
}

TEST_CASE("unknown argument errors with exit 2, never launching the GUI (rule 3)") {
    Harness h;
    REQUIRE(h.run({"exprot"}) == 2);
    REQUIRE(h.errText.contains("unknown command 'exprot'"));
    REQUIRE(h.launches.isEmpty());
}

TEST_CASE("an existing file as the unknown argument gets the ui suggestion") {
    QTemporaryDir tmp;
    const QString file = QDir(tmp.path()).filePath("shot.arw");
    { QFile f(file); REQUIRE(f.open(QIODevice::WriteOnly)); }
    const QByteArray fileBytes = file.toLocal8Bit();

    Harness h;
    REQUIRE(h.run({fileBytes.constData()}) == 2);
    REQUIRE(h.errText.contains("arraw ui"));
}

TEST_CASE("export usage errors surface through dispatch") {
    Harness h;
    REQUIRE(h.run({"export"}) == 2); // no inputs, no -o
    REQUIRE(h.errText.contains("arraw export"));
}
