#include "DebugLog.h"
#include "PhotoLoader.h"
#include "support/Fixtures.h"
#include "support/LensFixtures.h"
#include "support/Lensfun.h"
#include "support/TempDir.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/StatusLine.h"

#include <ImageImport.h>
#include <LensResolution.h>
#include <Photo.h>
#include <Sidecar.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The lens rows of the Negative group, and what a toggle does in the window (ADR 050, step 7).

namespace {

/// Decodes as the window does, counting.
struct CountingDecoder {
    std::atomic<int> started{0};

    [[nodiscard]] PhotoLoader::Decoder decoder() {
        return [this, real = PhotoLoader::imageDecoder()](const std::filesystem::path& path,
                                                          const DecodeOptions& options,
                                                          ProgressChannel& channel) {
            ++started;
            return real(path, options, channel);
        };
    }
};

/// A window over a folder of two RAWs that name the reference body's kit lens, a RAW that names no
/// lens, and a PNG, with the fixture lens database, optionally held back until released.
struct LensWindow {
    test::TempDir folder;
    DebugLog debugLog;
    CountingDecoder counter;
    std::atomic<bool> released;
    MainWindow window;

    explicit LensWindow(bool hold = false)
        : released(!hold),
          window(debugLog, nullptr, counter.decoder(), [this]() -> const LensDatabase& {
              while (!released) {
                  QThread::msleep(1);
              }
              return test::fixtureDatabase();
          }) {
        QSettings().remove("copySettings/sections");
        const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
        std::filesystem::copy_file(fixtures / "lens-canon-32x24.dng", folder.file("a.dng"));
        std::filesystem::copy_file(fixtures / "lens-canon-32x24.dng", folder.file("b.dng"));
        std::filesystem::copy_file(fixtures / "bayer-32x24.dng", folder.file("c.dng"));
        std::filesystem::copy_file(fixtures / "testcard-61x41-srgb8.png", folder.file("d.png"));
        window.resize(1200, 800);
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] FilmStrip& strip() const {
        return *window.findChild<FilmStrip*>();
    }

    [[nodiscard]] StatusLine& status() const {
        return *window.findChild<StatusLine*>();
    }

    [[nodiscard]] QAction& action(const QString& name) const {
        for (QAction* candidate : window.findChildren<QAction*>()) {
            if (candidate->objectName() == name) {
                return *candidate;
            }
        }
        FAIL("no action " << name.toStdString());
        throw;
    }

    [[nodiscard]] QCheckBox& box(const char* name) const {
        auto* found = window.findChild<QCheckBox*>(name);
        REQUIRE(found != nullptr);
        return *found;
    }

    [[nodiscard]] QCheckBox& distortion() const {
        return box("lensDistortionCheck");
    }

    [[nodiscard]] QString lensStatus() const {
        return window.findChild<QLabel*>("lensStatus")->text();
    }

    [[nodiscard]] QPushButton& button(const char* name) const {
        auto* found = window.findChild<QPushButton*>(name);
        REQUIRE(found != nullptr);
        return *found;
    }

    /// Opens a shot and waits for its first render.
    void open(const char* name) const {
        emit strip().activationRequested(QString::fromStdU16String(folder.file(name).u16string()));
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
    }

    /// Waits for the lens rows to be settable.
    [[nodiscard]] bool ready() const {
        return QTest::qWaitFor([this] { return distortion().isEnabled(); }, 20000);
    }

    [[nodiscard]] int historyRows() const {
        return window.findChild<QListView*>("historyList")->model()->rowCount();
    }

    /// Triggers Copy and accepts its dialog.
    void copy() const {
        QTimer::singleShot(0, [] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                dialog->accept();
            }
        });
        action("copySettingsAction").trigger();
    }

    [[nodiscard]] Photo saved(const char* name) const {
        return openPhoto(folder.file(name));
    }
};

} // namespace

TEST_CASE("The lens switches are disabled until the database has loaded", "[app][window][lens]") {
    LensWindow w(true);
    w.open("a.dng");
    CHECK_FALSE(w.distortion().isEnabled());
    CHECK_FALSE(w.box("lensVignettingCheck").isEnabled());
    CHECK_FALSE(w.box("lensCaCheck").isEnabled());
    CHECK_FALSE(w.button("lensChooseButton").isEnabled());
    CHECK(w.lensStatus().contains("Loading"));

    w.released = true;
    REQUIRE(w.ready());
    CHECK(w.box("lensVignettingCheck").isEnabled());
    CHECK_FALSE(w.lensStatus().contains("Loading"));
}

TEST_CASE("A lens toggle never decodes again, and is one step that records the lens",
          "[app][window][lens][lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensWindow w;
    w.open("a.dng");
    REQUIRE(w.ready());
    CHECK(w.counter.started == 1);
    // Off by default; the lens is found, and not yet recorded.
    CHECK_FALSE(w.distortion().isChecked());
    CHECK(w.lensStatus().contains("EF-S 18-135mm"));
    CHECK(w.lensStatus().contains("automatically"));
    const QImage before = w.view().wholeFrameImage();
    const int rows = w.historyRows();

    w.distortion().click();
    REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage() != before; }, 20000));
    CHECK(w.distortion().isChecked());
    CHECK(w.historyRows() == rows + 1);
    // The step recorded the match with it.
    CHECK(w.lensStatus().contains("recorded"));
    CHECK(w.window.findChild<QPushButton*>("lensResetButton")->isEnabled());
    const QImage corrected = w.view().wholeFrameImage();

    // More toggles, their undo and redo: one decode throughout.
    w.box("lensVignettingCheck").click();
    REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage() != corrected; }, 20000));
    CHECK(w.historyRows() == rows + 2);
    w.action("undoAction").trigger();
    REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage() == corrected; }, 20000));
    w.action("undoAction").trigger(); // The distortion, and the record with it.
    REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage() == before; }, 20000));
    CHECK_FALSE(w.distortion().isChecked());
    CHECK(w.lensStatus().contains("automatically"));
    CHECK_FALSE(w.window.findChild<QPushButton*>("lensResetButton")->isEnabled());
    w.action("redoAction").trigger();
    REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage() == corrected; }, 20000));
    CHECK(w.lensStatus().contains("recorded"));
    QTest::qWait(100);
    CHECK(w.counter.started == 1);
}

TEST_CASE("Turning the switches off keeps the recorded lens", "[app][window][lens][lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensWindow w;
    w.open("a.dng");
    REQUIRE(w.ready());
    const QImage before = w.view().wholeFrameImage();
    w.distortion().click();
    REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage() != before; }, 20000));
    w.distortion().click();
    REQUIRE(QTest::qWaitFor([&] { return w.view().wholeFrameImage() == before; }, 20000));
    CHECK(w.lensStatus().contains("recorded"));
}

TEST_CASE("A lens the database does not list says so, and the switches stay as set",
          "[app][window][lens][lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensWindow w;
    w.open("c.dng"); // Names no lens.
    REQUIRE(w.ready());
    CHECK(w.lensStatus().startsWith("No profile"));
    CHECK(w.window.findChild<QPushButton*>("lensChooseButton")->isEnabled());

    const QImage before = w.view().wholeFrameImage();
    w.distortion().click();
    QTest::qWait(150);
    // The correction is skipped, not refused: the picture is what it was, the switch is on.
    CHECK(w.distortion().isChecked());
    CHECK(w.lensStatus().startsWith("No profile"));
    CHECK(w.view().wholeFrameImage() == before);
    CHECK(w.counter.started == 1);
}

TEST_CASE("The lens rows are off for a file that is not a RAW", "[app][window][lens]") {
    LensWindow w;
    w.open("d.png");
    CHECK_FALSE(w.distortion().isEnabled());
    CHECK(w.lensStatus().contains("RAW"));
}

TEST_CASE("A paste of the lens switches records the target's own lens, in one step with no decode",
          "[app][window][lens][lensfun][copy]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensWindow w;
    {
        // The photograph copied from has distortion on and a lens of its own recorded.
        const Photo photo = w.saved("a.dng");
        NegativeSettings negative = photo.negative();
        negative.lens.distortion = true;
        negative.lens.lens = RecordedLens{"Canon", "Canon EF-S 18-135mm f/3.5-5.6 IS USM", 1.6,
                                          LensOrigin::Chosen, "an old stamp"};
        writeSidecar(photo.with(negative));
    }
    w.open("a.dng");
    REQUIRE(w.ready());
    w.copy();
    w.open("b.dng");
    REQUIRE(w.ready());
    const int before = w.counter.started;
    const int rows = w.historyRows();

    w.action("pasteSettingsAction").trigger();
    QTest::qWait(200);
    CHECK(w.historyRows() == rows + 1);
    CHECK(w.counter.started == before);
    CHECK(w.distortion().isChecked());
    // The target matched its own lens: recorded as matched here, not the one copied from.
    CHECK(w.lensStatus().contains("recorded"));
    w.action("saveAction").trigger();
    const NegativeSettings saved = w.saved("b.dng").negative();
    REQUIRE(saved.lens.lens.has_value());
    CHECK(saved.lens.lens->origin == LensOrigin::Matched);
    CHECK(saved.lens.lens->database == test::fixtureDatabase().stamp());
}

TEST_CASE("A paste that needs the lens database waits for it rather than record no lens",
          "[app][window][lens][lensfun][copy]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensWindow w(true);
    {
        const Photo photo = w.saved("a.dng");
        NegativeSettings negative = photo.negative();
        negative.lens.distortion = true;
        writeSidecar(photo.with(negative));
    }
    emit w.strip().activationRequested(
        QString::fromStdU16String(w.folder.file("a.dng").u16string()));
    QTest::qWait(500);
    w.copy();
    w.open("b.dng");
    const int rows = w.historyRows();
    w.action("pasteSettingsAction").trigger();
    QTest::qWait(100);
    CHECK(w.historyRows() == rows);
    CHECK_FALSE(w.distortion().isChecked());

    w.released = true;
    REQUIRE(w.ready());
    w.action("pasteSettingsAction").trigger();
    QTest::qWait(200);
    CHECK(w.historyRows() == rows + 1);
    CHECK(w.distortion().isChecked());
    CHECK(w.lensStatus().contains("recorded"));
}

TEST_CASE("A sidecar that turns a correction on renders with it once the database lands",
          "[app][window][lens][lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensWindow w(true);
    QImage plain;
    {
        const Photo photo = w.saved("a.dng");
        NegativeSettings negative = photo.negative();
        negative.lens.distortion = true;
        negative.lens.vignetting = true;
        writeSidecar(photo.with(negative));
    }
    w.open("c.dng");
    plain = w.view().wholeFrameImage();
    emit w.strip().activationRequested(
        QString::fromStdU16String(w.folder.file("a.dng").u16string()));
    // Nothing is shown of a photograph that asks for corrections the database has to resolve.
    QTest::qWait(300);
    CHECK(w.view().wholeFrameImage().isNull());
    w.released = true;
    REQUIRE(QTest::qWaitFor([&] { return !w.view().wholeFrameImage().isNull(); }, 20000));
    CHECK(w.distortion().isChecked());
    CHECK(w.counter.started == 2);
}

TEST_CASE("Choosing a lens by hand records it as chosen, and resetting matches it again",
          "[app][window][lens][lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    LensWindow w;
    w.open("a.dng");
    REQUIRE(w.ready());
    w.distortion().click();
    QTest::qWait(150);

    // The picker lists the lenses of the camera's mount; the second is taken.
    QString chosenName;
    QTimer::singleShot(200, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialog != nullptr);
        auto* list = dialog->findChild<QListWidget*>("lensList");
        REQUIRE(list != nullptr);
        REQUIRE(list->count() >= 2);
        list->setCurrentRow(1);
        chosenName = list->currentItem()->text();
        dialog->accept();
    });
    w.button("lensChooseButton").click();
    REQUIRE(QTest::qWaitFor([&] { return !chosenName.isEmpty(); }, 20000));
    QTest::qWait(150);
    CHECK(w.lensStatus().contains("chosen"));
    CHECK(w.lensStatus().contains(chosenName.left(chosenName.indexOf('(')).trimmed()));

    w.button("lensResetButton").click();
    QTest::qWait(150);
    CHECK(w.lensStatus().contains("recorded"));
    CHECK_FALSE(w.lensStatus().contains("chosen"));
    CHECK(w.counter.started == 1);
}

TEST_CASE("A lens with no calibration for the correction is found, and the status says why",
          "[app][lens][lensfun]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const test::TempDir dir;
    const auto raw = dir.file("frame.dng");
    std::filesystem::copy_file(test::fixture("lens-canon-32x24.dng"), raw);
    const Photo plain = openPhoto(raw);
    ImageMetadata bare = plain.metadata();
    bare.lens.lensNames = {"Sigma 18-35mm F1.8 DC HSM"};
    bare.lens.minFocal = 18.0;
    bare.lens.maxFocal = 35.0;
    bare.lens.maxAperture = 1.8;
    bare.lens.focalLength = 24.0;
    NegativeSettings negative = plain.negative();
    negative.lens.vignetting = true; // The fixture lens has none.
    const LensView view =
        resolveLensView(Photo(raw, bare, negative, plain.state()), &test::fixtureDatabase());
    CHECK_FALSE(view.correction.active());
    CHECK(view.status.found);
    CHECK(view.status.text.contains("Sigma"));
    CHECK_FALSE(view.status.text.contains("No lens was found"));
    CHECK(view.status.note.contains("vignetting"));
}

/// Hidden: what a lens toggle costs on a large photograph (ADR 050, step 7). Run it on a release
/// build with `ARRAW_BENCH_RAW` naming a RAW of about 24 MP that the system database knows the
/// lens of, and `QT_QPA_PLATFORM=offscreen`; it prints, and asserts nothing.
TEST_CASE("The cost of a lens toggle in the window", "[.lensbench]") {
    const char* raw = std::getenv("ARRAW_BENCH_RAW");
    if (raw == nullptr) {
        SKIP("ARRAW_BENCH_RAW names no RAW");
    }
    using Clock = std::chrono::steady_clock;
    const auto milliseconds = [](Clock::duration span) {
        return std::chrono::duration<double, std::milli>(span).count();
    };

    // What the interface thread does around a toggle, with the full database.
    auto start = Clock::now();
    const LensDatabase& database = LensDatabase::system();
    WARN("system database: " << milliseconds(Clock::now() - start) << " ms, "
                             << database.lensCount() << " lenses");
    const Photo photo = openPhoto(raw);
    NegativeSettings negative = photo.negative();
    negative.lens.distortion = true;
    start = Clock::now();
    constexpr int rounds = 20;
    for (int round = 0; round < rounds; ++round) {
        static_cast<void>(resolveLensView(photo.with(negative), &database));
    }
    WARN("resolveLensView, matching included: " << milliseconds(Clock::now() - start) / rounds
                                                << " ms");
    start = Clock::now();
    for (int round = 0; round < rounds; ++round) {
        static_cast<void>(recordLensMatch(photo.with(negative), database));
    }
    WARN("recordLensMatch: " << milliseconds(Clock::now() - start) / rounds << " ms");
    NegativeSettings recorded = recordLensMatch(photo.with(negative), database).negative();
    start = Clock::now();
    for (int round = 0; round < rounds; ++round) {
        static_cast<void>(resolveLensView(photo.with(recorded), &database));
    }
    WARN("resolveLensView, recorded lens: " << milliseconds(Clock::now() - start) / rounds
                                            << " ms");

    // The window around it: the longest gap of the interface thread, and the time to the picture.
    DebugLog debugLog;
    CountingDecoder counter;
    MainWindow window(debugLog, nullptr, counter.decoder());
    window.resize(1200, 800);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    auto* strip = window.findChild<FilmStrip*>();
    auto* view = window.findChild<PhotoView*>();
    auto* check = window.findChild<QCheckBox*>("lensDistortionCheck");
    emit strip->activationRequested(QString::fromUtf8(raw));
    REQUIRE(QTest::qWaitFor([&] { return !view->wholeFrameImage().isNull(); }, 120000));
    REQUIRE(QTest::qWaitFor([&] { return check->isEnabled(); }, 60000));
    QTest::qWait(500);

    Clock::time_point last = Clock::now();
    double longest = 0.0;
    QTimer ticker;
    ticker.setInterval(2);
    QObject::connect(&ticker, &QTimer::timeout, [&] {
        const auto now = Clock::now();
        longest = std::max(longest, milliseconds(now - last));
        last = now;
    });
    for (const bool on : {true, false, true}) {
        const QImage before = view->wholeFrameImage();
        longest = 0.0;
        last = Clock::now();
        ticker.start();
        const auto clicked = Clock::now();
        check->click();
        REQUIRE(QTest::qWaitFor([&] { return view->wholeFrameImage() != before; }, 120000));
        ticker.stop();
        WARN("distortion " << (on ? "on" : "off") << ": " << milliseconds(Clock::now() - clicked)
                           << " ms to the picture, longest gap of the interface thread " << longest
                           << " ms");
        QTest::qWait(300);
    }
    SUCCEED();
}
