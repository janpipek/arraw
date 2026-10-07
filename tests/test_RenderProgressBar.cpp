#include "DebugLog.h"
#include "support/TempDir.h"
#include "ui/CropOverlay.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/RenderProgressBar.h"

#include <QLabel>
#include <QProgressBar>
#include <QStatusBar>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace arraw;
using namespace arraw::app;

/// The step and the progress bar in the status bar (ADR 042).

namespace {

RenderActivity::Display displayOf(std::optional<double> fraction) {
    return {.visible = true, .fraction = fraction, .step = ProgressStep::Denoise};
}

} // namespace

TEST_CASE("The widget shows the step and the fraction, and hides with the display",
          "[app][progress][bar]") {
    RenderProgressBar widget;
    CHECK(widget.isHidden());

    widget.setDisplay(displayOf(0.4));
    REQUIRE(widget.isVisible());
    CHECK(widget.stepLabel().text() == QString::fromUtf8("Reducing noise…"));
    CHECK(widget.bar().maximum() == RenderProgressBar::resolution);
    CHECK(widget.bar().value() == 400);

    // Without a fraction the bar is busy, and back to a fraction when one comes.
    widget.setDisplay(displayOf({}));
    CHECK(widget.bar().minimum() == 0);
    CHECK(widget.bar().maximum() == 0);
    widget.setDisplay(displayOf(1.0));
    CHECK(widget.bar().value() == RenderProgressBar::resolution);

    widget.setDisplay({});
    CHECK_FALSE(widget.isVisible());
}

TEST_CASE("The window shows a long render's step and bar, and hides them when it is done",
          "[app][window][progress]") {
    test::TempDir folder;
    const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
    std::filesystem::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
    DebugLog debugLog;
    MainWindow window(debugLog);
    window.openInitialPath(folder.file("a.dng"));
    window.show();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    auto& view = *window.findChild<PhotoView*>();
    REQUIRE(QTest::qWaitFor([&] { return !view.wholeFrameImage().isNull(); }, 20000));
    auto* label = window.findChild<QLabel*>("renderStepLabel");
    REQUIRE(label != nullptr);
    auto& progress = *window.findChild<RenderProgressBar*>();
    // In the status bar, not over the photograph.
    CHECK(progress.parentWidget() != &view);
    CHECK(view.findChild<RenderProgressBar*>() == nullptr);
    // The first render was over in a moment.
    QTest::qWait(700);
    CHECK_FALSE(progress.isVisible());
    CHECK_FALSE(label->isVisible());

    // A render that takes its time: the bar waits out the delay, then shows.
    RenderIndicator& indicator = window.renderIndicator();
    indicator.begin();
    CHECK_FALSE(progress.isVisible());
    CHECK(QTest::qWaitFor([&] { return progress.isVisible(); }, 2000));
    CHECK(label->isVisible());
    CHECK(label->text() == QString::fromUtf8("Developing…"));
    // Beside an export's message, not in its place.
    window.statusBar()->showMessage("Exporting a.dng…");
    CHECK(label->isVisible());
    window.statusBar()->clearMessage();

    indicator.report(0.4, ProgressStep::Denoise);
    CHECK(progress.display().fraction == 0.4);
    CHECK(label->text() == QString::fromUtf8("Reducing noise…"));

    // In the crop mode as well.
    view.setCropMode(true);
    CHECK(progress.isVisible());
    view.setCropMode(false);

    indicator.finish();
    CHECK(QTest::qWaitFor([&] { return !progress.isVisible(); }, 2000));
    CHECK_FALSE(label->isVisible());
}

TEST_CASE("The window's renders begin and end the busy period, and stale progress is ignored",
          "[app][window][progress]") {
    test::TempDir folder;
    const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
    std::filesystem::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
    DebugLog debugLog;
    MainWindow window(debugLog);
    window.openInitialPath(folder.file("a.dng"));
    window.show();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    auto& view = *window.findChild<PhotoView*>();
    REQUIRE(QTest::qWaitFor([&] { return !view.wholeFrameImage().isNull(); }, 20000));
    RenderIndicator& indicator = window.renderIndicator();
    auto& progress = *window.findChild<RenderProgressBar*>();
    // The first render's image ended its busy period.
    REQUIRE(QTest::qWaitFor([&] { return !indicator.busy(); }, 20000));

    // A new size asks for a render, which begins one; its image ends it.
    window.resize(window.size() + QSize(40, 30));
    REQUIRE(QTest::qWaitFor([&] { return indicator.busy(); }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return !indicator.busy(); }, 20000));

    // Progress of a request that is not the newest does not reach the bar.
    indicator.begin();
    REQUIRE(QTest::qWaitFor([&] { return progress.isVisible(); }, 2000));
    window.showRenderProgress(0, 0.5, ProgressStep::Denoise);
    CHECK_FALSE(progress.display().fraction.has_value());
    indicator.finish(false);
    CHECK(QTest::qWaitFor([&] { return !progress.isVisible(); }, 2000));
}
