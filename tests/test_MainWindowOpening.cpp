#include "ThumbnailCache.h"
#include "ThumbnailWorker.h"
#include "TimingTrace.h"
#include "support/TempDir.h"
#include "ui/CropOverlay.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"

#include <Photo.h>
#include <Sidecar.h>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <system_error>

using namespace arraw;
using namespace arraw::app;

namespace {

/// Longest gap between two turns of the event loop while a probe runs.
class StallProbe {
public:
    StallProbe() {
        timer_.setInterval(1);
        QObject::connect(&timer_, &QTimer::timeout, [this] {
            const qint64 now = clock_.nsecsElapsed();
            if (now - last_ > 12'000'000) {
                const detail::TimingSpan stall("bench.stall");
                stall.note(std::to_string((now - last_) / 1000000) + " ms");
            }
            longest_ = std::max(longest_, now - last_);
            last_ = now;
        });
    }

    void start() {
        longest_ = 0;
        clock_.start();
        last_ = 0;
        timer_.start();
    }

    /// @brief Longest gap so far, including the one still open, in milliseconds.
    [[nodiscard]] double longestMs() const {
        return static_cast<double>(std::max(longest_, clock_.nsecsElapsed() - last_)) / 1e6;
    }

private:
    QTimer timer_;
    QElapsedTimer clock_;
    qint64 last_ = 0;
    qint64 longest_ = 0;
};

QAction* findAction(const MainWindow& window, const QString& text) {
    for (QAction* candidate : window.findChildren<QAction*>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    return nullptr;
}

/// A window over a folder of three photographs, each with a sidecar of its own exposure.
struct Window {
    test::TempDir folder;
    MainWindow window;

    Window() {
        const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
        addShot(fixtures / "preview-32x24.dng", "a.dng", 0.5F);
        addShot(fixtures / "bayer-32x24.dng", "b.dng", 1.0F);
        addShot(fixtures / "linear-32x24-rotated.dng", "c.dng", 1.5F);
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
    }

    void addShot(const std::filesystem::path& from, const char* name, float exposure) const {
        const std::filesystem::path path = folder.file(name);
        std::filesystem::copy_file(from, path);
        const Photo bare = openPhoto(path);
        DevelopState state = bare.state();
        state.settings.tone.exposure = exposure;
        writeSidecar(Photo(path, bare.metadata(), state));
    }

    [[nodiscard]] std::filesystem::path shot(const char* name) const {
        return folder.file(name);
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] FilmStrip& strip() const {
        return *window.findChild<FilmStrip*>();
    }

    [[nodiscard]] QWidget& developDock() const {
        for (auto* dock : window.findChildren<QDockWidget*>()) {
            if (dock->findChild<DevelopPanel*>() != nullptr) {
                return *dock;
            }
        }
        FAIL("no develop dock");
        throw;
    }

    /// Exposure the panel shows.
    [[nodiscard]] double panelExposure() const {
        for (auto* row : window.findChildren<SettingSlider*>()) {
            if (row->key() == "exposure") {
                return row->findChild<QDoubleSpinBox*>()->value();
            }
        }
        FAIL("no exposure row");
        throw;
    }

    /// Asks the strip to open a shot, as a click on its cell does.
    void activate(const char* name) const {
        emit strip().activationRequested(QString::fromStdU16String(shot(name).u16string()));
    }

    /// Waits for the first render of the photograph being opened.
    [[nodiscard]] bool waitForRender() const {
        return QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000);
    }
};

} // namespace

TEST_CASE("Opening a photograph returns before it is decoded, and edits wait for the pixels",
          "[app][window][opening]") {
    Window w;
    w.window.openInitialPath(w.shot("a.dng"));
    // No event has been processed since: the decode cannot have landed, yet the photograph is
    // open, and the panel shows its sidecar's state.
    CHECK(w.window.windowTitle().startsWith("a.dng"));
    CHECK(w.panelExposure() == 0.5);
    CHECK(w.view().wholeFrameImage().isNull());
    CHECK_FALSE(w.developDock().isEnabled());
    CHECK_FALSE(findAction(w.window, "&Crop && Straighten")->isEnabled());
    CHECK_FALSE(findAction(w.window, "&Export…")->isEnabled());
    CHECK_FALSE(findAction(w.window, "Rotate &Right")->isEnabled());
    // A crop asked for now is not entered: there is nothing to crop yet.
    findAction(w.window, "&Crop && Straighten")->trigger();
    CHECK_FALSE(w.view().isCropMode());

    REQUIRE(w.waitForRender());
    CHECK(w.developDock().isEnabled());
    CHECK(findAction(w.window, "&Crop && Straighten")->isEnabled());
    CHECK(findAction(w.window, "&Export…")->isEnabled());
    CHECK(w.panelExposure() == 0.5);
}

TEST_CASE("Switching photographs quickly ends on the last one, whole", "[app][window][opening]") {
    Window w;
    w.window.openInitialPath(w.shot("a.dng"));
    REQUIRE(w.waitForRender());
    const QImage first = w.view().wholeFrameImage();

    // Each replaces the one before, whose decode is cancelled or dropped.
    w.activate("b.dng");
    w.activate("c.dng");
    w.activate("b.dng");
    w.activate("c.dng");
    // The previous photograph's picture went at once: never shown for another.
    CHECK(w.view().wholeFrameImage().isNull());
    CHECK(w.window.windowTitle().startsWith("c.dng"));
    CHECK(w.panelExposure() == 1.5);
    REQUIRE(w.waitForRender());
    // Whatever was still on its way of the others is dropped, not shown.
    QTest::qWait(300);
    CHECK(w.window.windowTitle().startsWith("c.dng"));
    CHECK(w.strip().activePrimary() == w.shot("c.dng"));
    CHECK(w.panelExposure() == 1.5);
    CHECK(w.developDock().isEnabled());
    // The rotated fixture's frame is portrait; a and b are landscape.
    const QImage shown = w.view().wholeFrameImage();
    CHECK(shown.height() > shown.width());
    CHECK(shown != first);
}

TEST_CASE("Entering the crop mode reads the camera preview off the GUI thread",
          "[app][window][opening][crop]") {
    Window w;
    w.window.openInitialPath(w.shot("a.dng"));
    REQUIRE(w.waitForRender());
    auto& overlay = *w.window.findChild<CropOverlay*>();
    findAction(w.window, "&Crop && Straighten")->trigger();
    REQUIRE(w.view().isCropMode());
    // No event has been processed: the developed frame stands in, the preview is not read yet.
    CHECK(overlay.showsPhotograph());
    CHECK(overlay.image().isNull());
    // The preview, or the render that follows it, then fills the whole photograph.
    REQUIRE(QTest::qWaitFor([&] { return !overlay.image().isNull(); }, 20000));
    findAction(w.window, "&Crop && Straighten")->trigger();
    CHECK_FALSE(w.view().isCropMode());
}

/// Measures the GUI thread's longest stall in each interaction on a folder of large photographs
/// (ADR 043). ARRAW_BENCH_FOLDER names the folder; its first two shots are opened in turn.
TEST_CASE("The window's interactions stall the GUI thread no longer than measured",
          "[.bench][app][window]") {
    const char* folderVariable = std::getenv("ARRAW_BENCH_FOLDER");
    if (folderVariable == nullptr) {
        SKIP("ARRAW_BENCH_FOLDER is not set");
    }
    const std::filesystem::path folder(folderVariable);
    MainWindow window;
    window.resize(1600, 1000);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    auto& view = *window.findChild<PhotoView*>();
    auto& strip = *window.findChild<FilmStrip*>();
    StallProbe probe;
    const auto measure = [&](const char* what, const std::function<void()>& act,
                             const std::function<bool()>& done) {
        probe.start();
        QElapsedTimer total;
        total.start();
        act();
        const double returned = static_cast<double>(total.nsecsElapsed()) / 1e6;
        REQUIRE(QTest::qWaitFor(done, 60000));
        std::cout << what << ": call " << returned << " ms, longest stall " << probe.longestMs()
                  << " ms, until done " << static_cast<double>(total.nsecsElapsed()) / 1e6
                  << " ms\n";
    };

    QImage shown;
    measure(
        "open folder", [&] { window.openInitialPath(folder); },
        [&] { return !view.wholeFrameImage().isNull() && view.wholeFrameImage() != shown; });
    QTest::qWait(3000); // Thumbnails and marks settle.
    shown = view.wholeFrameImage();
    const auto second = strip.firstVisible();
    REQUIRE(second);
    // The second shot of the folder.
    std::filesystem::path next;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path() != *second && entry.path().extension() == ".dng") {
            next = entry.path();
            break;
        }
    }
    measure(
        "switch photo",
        [&] { emit strip.activationRequested(QString::fromStdU16String(next.u16string())); },
        [&] { return !view.wholeFrameImage().isNull() && view.wholeFrameImage() != shown; });
    QTest::qWait(500);
    // The first entry on a cold cache reads the camera's preview from the file.
    {
        std::error_code ignored;
        std::filesystem::remove_all(ThumbnailCache::defaultRoot(), ignored);
        const test::TempDir empty;
        QElapsedTimer read;
        read.start();
        const QImage preview = embeddedPreviewImage(ThumbnailCache(empty.path()), next);
        std::cout << "embedded preview, cold: " << static_cast<double>(read.nsecsElapsed()) / 1e6
                  << " ms, " << preview.width() << "x" << preview.height() << "\n";
    }
    measure(
        "enter crop", [&] { findAction(window, "&Crop && Straighten")->trigger(); },
        [&] { return view.isCropMode(); });
    QTest::qWait(1000);
    measure(
        "leave crop", [&] { findAction(window, "&Crop && Straighten")->trigger(); },
        [&] { return !view.isCropMode(); });
    QTest::qWait(500);
    measure("rotate", [&] { findAction(window, "Rotate &Right")->trigger(); }, [] { return true; });
    QTest::qWait(1000);
    measure(
        "save", [&] { findAction(window, "&Save Adjustments")->trigger(); }, [] { return true; });
    QTest::qWait(500);
}
