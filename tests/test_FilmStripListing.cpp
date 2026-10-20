#include "DebugLog.h"
#include "support/ListingGate.h"
#include "support/TempDir.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"

#include <QLabel>
#include <QListView>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace arraw;
using namespace arraw::app;

/// The film strip while its folder is listed in the background (incremental-refresh plan,
/// step 3): the shot to activate that waits for its folder, the first shot, the busy note and
/// the empty hints, and the window's ways of opening.

namespace {

namespace fs = std::filesystem;

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

QString text(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// A strip whose thumbnails go to a folder of the test's own, and a gate on its listing. The
/// gate is declared last, so that it lets the walk go before the strip waits for its thread.
struct GatedStrip {
    test::TempDir cache;
    FilmStrip strip{nullptr, ThumbnailCache(cache.path())};
    test::ListingGate gate;

    explicit GatedStrip(fs::path at) : gate(std::move(at)) {
        strip.setListingHook(gate.hook());
    }

    [[nodiscard]] QLabel& label() const {
        return *strip.titleBar()->findChild<QLabel*>("filmStripFolderLabel");
    }

    [[nodiscard]] QLabel& hint() const {
        return *strip.findChild<QLabel*>("filmStripEmptyHint");
    }

    [[nodiscard]] bool listed() const {
        return QTest::qWaitFor([this] { return !strip.isListing(); }, 10000);
    }
};

} // namespace

TEST_CASE("A shot to activate waits for its folder to be listed", "[app][filmstrip][listing]") {
    const test::TempDir folder;
    touch(folder.file("a.jpg"));
    touch(folder.file("deep/er/b.dng"));
    touch(folder.file("deep/er/b.jpg"));
    touch(folder.file("z/c.jpg"));
    GatedStrip holder(folder.file("deep/er"));
    FilmStrip& strip = holder.strip;
    strip.setIncludeSubfolders(true);
    QSignalSpy requested(&strip, &FilmStrip::activationRequested);

    SECTION("Given to setFolder, as a companion") {
        strip.setFolder(folder.path(), folder.file("deep/er/b.jpg"));
        REQUIRE(QTest::qWaitFor([&] { return holder.gate.reached(); }, 10000));
        REQUIRE(QTest::qWaitFor(
            [&] { return strip.shotContaining(folder.file("a.jpg")).has_value(); }, 10000));
        CHECK_FALSE(strip.activePrimary());
        holder.gate.release();
        REQUIRE(QTest::qWaitFor(
            [&] { return strip.activePrimary() == folder.file("deep/er/b.dng"); }, 10000));
        CHECK(strip.targets() == std::vector<fs::path>{folder.file("deep/er/b.dng")});
    }
    SECTION("Given to setActive during the listing; the last one wins") {
        strip.setFolder(folder.path());
        REQUIRE(QTest::qWaitFor([&] { return holder.gate.reached(); }, 10000));
        strip.setActive(folder.file("z/c.jpg"));
        strip.setActive(folder.file("deep/er/b.dng"));
        holder.gate.release();
        REQUIRE(holder.listed());
        CHECK(strip.activePrimary() == folder.file("deep/er/b.dng"));
    }
    SECTION("One the listing never brings leaves no shot active") {
        strip.setFolder(folder.path(), folder.file("deep/er/missing.dng"));
        holder.gate.release();
        REQUIRE(holder.listed());
        CHECK_FALSE(strip.activePrimary());
        // Gone with the listing: a later refresh does not make it active either.
        touch(folder.file("deep/er/missing.dng"));
        REQUIRE(QTest::qWaitFor(
            [&] { return strip.shotContaining(folder.file("deep/er/missing.dng")).has_value(); },
            10000));
        CHECK_FALSE(strip.activePrimary());
    }
    // The photograph is open already: the strip asks for nothing.
    CHECK(requested.isEmpty());
}

TEST_CASE("The strip announces the first shot it shows of a listing, once",
          "[app][filmstrip][listing]") {
    const test::TempDir folder;
    touch(folder.file("sub/b.jpg"));
    touch(folder.file("sub/c.jpg"));
    GatedStrip holder(folder.file("sub"));
    FilmStrip& strip = holder.strip;
    strip.setIncludeSubfolders(true);
    QSignalSpy first(&strip, &FilmStrip::firstShotListed);

    strip.setFolder(folder.path());
    // The root has no shots: nothing to announce until the subfolder is read.
    REQUIRE(QTest::qWaitFor([&] { return holder.gate.reached(); }, 10000));
    QCoreApplication::processEvents();
    CHECK(first.isEmpty());
    holder.gate.release();
    REQUIRE(holder.listed());
    REQUIRE(QTest::qWaitFor([&] { return first.count() == 1; }, 10000));
    CHECK(first.front().front().toString() == text(folder.file("sub/b.jpg")));

    // A refresh that brings a shot is no listing.
    touch(folder.file("a.jpg"));
    REQUIRE(QTest::qWaitFor([&] { return strip.shotContaining(folder.file("a.jpg")).has_value(); },
                            10000));
    QCoreApplication::processEvents();
    CHECK(first.count() == 1);
}

TEST_CASE("The strip says that it is listing, and what an empty strip means",
          "[app][filmstrip][listing]") {
    const test::TempDir folder;
    touch(folder.file("sub/b.jpg"));
    GatedStrip holder(folder.file("sub"));
    FilmStrip& strip = holder.strip;
    strip.resize(600, 200);
    strip.show();
    strip.setIncludeSubfolders(true);

    strip.setFolder(folder.path());
    REQUIRE(QTest::qWaitFor([&] { return holder.gate.reached(); }, 10000));
    CHECK(holder.hint().isVisible());
    CHECK(holder.hint().text() == "Listing folder…");
    // The note comes after the anti-flicker delay, and counts the shots so far.
    CHECK_FALSE(holder.label().text().contains("listing"));
    REQUIRE(
        QTest::qWaitFor([&] { return holder.label().text().contains("listing… 0 shots"); }, 10000));
    CHECK(holder.label().text().startsWith(text(folder.path()) + " (+ subfolders)"));

    holder.gate.release();
    REQUIRE(holder.listed());
    CHECK_FALSE(holder.label().text().contains("listing"));
    CHECK_FALSE(holder.hint().isVisible());

    // A folder without photographs says so once it is listed.
    const test::TempDir empty;
    strip.setFolder(empty.path());
    REQUIRE(holder.listed());
    CHECK(holder.hint().isVisible());
    CHECK(holder.hint().text() == "No photographs in this folder");
}

namespace {

/// Starts and ends with the subfolder flag and the remembered folder and file unset.
struct CleanSettings {
    static void clear() {
        QSettings store;
        store.remove("filmStrip/includeSubfolders");
        store.remove("lastFolder");
        store.remove("lastFile");
    }
    CleanSettings() {
        clear();
    }
    ~CleanSettings() {
        clear();
    }
};

/// A window over `a.dng` and `deep/er/b.dng`.
struct Window {
    CleanSettings settings;
    test::TempDir folder;
    DebugLog debugLog;
    MainWindow window{debugLog};

    Window() {
        const fs::path fixtures(ARRAW_TEST_DATA_DIR);
        fs::create_directories(folder.file("deep/er"));
        fs::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
        fs::copy_file(fixtures / "bayer-32x24.dng", folder.file("deep/er/b.dng"));
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
    }

    [[nodiscard]] FilmStrip& strip() const {
        return *window.findChild<FilmStrip*>();
    }

    [[nodiscard]] bool rendered() const {
        return QTest::qWaitFor(
            [this] { return !window.findChild<PhotoView*>()->wholeFrameImage().isNull(); }, 20000);
    }
};

} // namespace

TEST_CASE("The window opens a folder's first shot once the listing brings it",
          "[app][window][listing]") {
    Window w;
    const test::ListingGate gate(w.folder.file("deep/er"));
    w.strip().setListingHook(gate.hook());
    w.strip().setIncludeSubfolders(true);
    // Moves the root's shot out, so that the first shot is the one the gate holds back.
    fs::remove(w.folder.file("a.dng"));

    w.window.openInitialPath(w.folder.path());
    CHECK(QSettings().value("lastFolder").toString() == text(w.folder.path()));
    REQUIRE(QTest::qWaitFor([&] { return gate.reached(); }, 10000));
    CHECK_FALSE(w.strip().activePrimary());
    gate.release();
    REQUIRE(QTest::qWaitFor(
        [&] { return w.strip().activePrimary() == w.folder.file("deep/er/b.dng"); }, 10000));
    REQUIRE(w.rendered());
    CHECK(w.window.windowTitle().startsWith("b.dng"));
}

TEST_CASE("A photograph restored from below the strip's folder waits for its listing",
          "[app][window][listing]") {
    Window w;
    const test::ListingGate gate(w.folder.file("deep/er"));
    w.strip().setListingHook(gate.hook());
    w.strip().setIncludeSubfolders(true);
    QSettings().setValue("lastFolder", text(w.folder.path()));
    QSettings().setValue("lastFile", text(w.folder.file("deep/er/b.dng")));

    w.window.openInitialPath();
    // The photograph opens at once, in the strip's folder, which is still being listed.
    CHECK(w.window.windowTitle().startsWith("b.dng"));
    CHECK(w.strip().folder() == w.folder.path());
    CHECK(w.strip().isListing());
    gate.release();
    REQUIRE(QTest::qWaitFor(
        [&] { return w.strip().activePrimary() == w.folder.file("deep/er/b.dng"); }, 10000));
    // Not replaced by the folder's first shot.
    REQUIRE(w.rendered());
    CHECK(w.window.windowTitle().startsWith("b.dng"));
    CHECK(w.strip().shotContaining(w.folder.file("a.dng")));
}
