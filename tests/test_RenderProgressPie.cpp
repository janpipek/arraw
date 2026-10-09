#include "DebugLog.h"
#include "support/TempDir.h"
#include "ui/CropOverlay.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/RenderProgressPie.h"
#include "ui/ThemeColors.h"

#include <QStatusBar>
#include <QTest>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace arraw;
using namespace arraw::app;

/// The render pie in the status bar (ADR 042).

namespace {

RenderActivity::Display displayOf(std::optional<double> fraction) {
    return {.visible = true, .fraction = fraction, .step = ProgressStep::Denoise};
}

} // namespace

TEST_CASE("The pie is full when idle, and shows the fraction while rendering",
          "[app][progress][pie]") {
    RenderProgressPie widget;
    widget.show();
    CHECK(widget.isVisible());
    CHECK_FALSE(widget.rendering());
    CHECK(widget.filled() == 1.0);
    CHECK(widget.toolTip() == "Up to date");
    CHECK(widget.accessibleName() == "Render progress");
    CHECK(widget.accessibleDescription() == "Up to date");

    widget.setDisplay(displayOf(0.4));
    CHECK(widget.isVisible());
    CHECK(widget.rendering());
    CHECK(widget.filled() == Catch::Approx(0.4));
    CHECK(widget.toolTip() == QString::fromUtf8("Reducing noise\u2026 40%"));
    CHECK(widget.accessibleDescription() == widget.toolTip());

    // No fraction yet: an empty pie with the step alone.
    widget.setDisplay(displayOf({}));
    CHECK(widget.rendering());
    CHECK(widget.filled() == 0.0);
    CHECK(widget.toolTip() == QString::fromUtf8("Reducing noise\u2026"));

    widget.setDisplay(displayOf(1.0));
    CHECK(widget.filled() == 1.0);
    CHECK(widget.rendering());

    widget.setDisplay({});
    CHECK(widget.isVisible());
    CHECK_FALSE(widget.rendering());
    CHECK(widget.toolTip() == "Up to date");
}

TEST_CASE("A failed render shows a red ring until a render is shown", "[app][progress][pie]") {
    RenderProgressPie widget;
    widget.setFailed("out of memory");
    CHECK(widget.failed());
    CHECK(widget.filled() == 0.0);
    CHECK(widget.toolTip() == "Render failed: out of memory");
    CHECK(widget.accessibleDescription() == widget.toolTip());

    // An idle display does not clear it; a render on its way does.
    widget.setDisplay({});
    CHECK(widget.failed());
    widget.setDisplay(displayOf(0.4));
    CHECK_FALSE(widget.failed());
    CHECK(widget.filled() == Catch::Approx(0.4));

    widget.setDisplay({});
    widget.setFailed("again");
    widget.setPhotoOpen(false);
    CHECK_FALSE(widget.failed());
    CHECK(widget.toolTip() == "No photograph open");
}

TEST_CASE("With no photograph open the pie is an empty grey ring", "[app][progress][pie]") {
    RenderProgressPie widget;
    widget.setPhotoOpen(false);
    CHECK_FALSE(widget.photoOpen());
    CHECK(widget.stateColour() == theme::progressIdle);
    CHECK_FALSE(widget.rendering());
    CHECK(widget.filled() == 0.0);
    CHECK(widget.toolTip() == "No photograph open");
    CHECK(widget.accessibleDescription() == "No photograph open");

    // A render shown is still shown as one.
    widget.setDisplay(displayOf(0.4));
    CHECK(widget.filled() == Catch::Approx(0.4));
    CHECK(widget.toolTip() == QString::fromUtf8("Reducing noise\u2026 40%"));

    widget.setDisplay({});
    widget.setPhotoOpen(true);
    CHECK(widget.filled() == 1.0);
    CHECK(widget.toolTip() == "Up to date");
}

TEST_CASE("A render shown clears a failure even if it never became visible",
          "[app][progress][pie]") {
    RenderProgressPie widget;
    widget.setFailed("bad");
    widget.setDisplay({});
    CHECK(widget.failed());
    widget.setOpened();
    CHECK_FALSE(widget.failed());
    CHECK(widget.filled() == 1.0);
    CHECK(widget.toolTip() == "Up to date");
}

TEST_CASE("An opening photograph is never up to date", "[app][progress][pie]") {
    RenderProgressPie widget;
    widget.setPhotoOpen(true);
    widget.setOpening("a.dng");
    CHECK(widget.opening());
    // Within the show delay: an empty dim green ring, neither full green nor grey.
    CHECK_FALSE(widget.rendering());
    CHECK(widget.stateColour() == theme::progressOpening);
    CHECK(widget.stateColour() != theme::progressDone);
    CHECK(widget.stateColour() != theme::progressIdle);
    CHECK(widget.filled() == 0.0);
    CHECK(widget.toolTip() == QString::fromUtf8("Opening a.dng\u2026"));

    widget.setDisplay(displayOf({}));
    CHECK(widget.stateColour() == theme::progressBusy);
    CHECK(widget.filled() == 0.0);
    CHECK(widget.toolTip() == QString::fromUtf8("Opening a.dng\u2026"));
    widget.setDisplay(displayOf(0.4));
    CHECK(widget.filled() == Catch::Approx(0.4));
    CHECK(widget.toolTip() == QString::fromUtf8("Opening a.dng\u2026 40%"));

    // The first render shown ends it; the indicator's hold keeps the render's own wording.
    widget.setOpened();
    CHECK_FALSE(widget.opening());
    CHECK(widget.toolTip() == QString::fromUtf8("Reducing noise\u2026 40%"));
    widget.setDisplay({});
    CHECK(widget.filled() == 1.0);
    CHECK(widget.toolTip() == "Up to date");

    // A failure shows the error but does not end it: nothing has been rendered yet. A closed
    // photograph ends it.
    widget.setOpening("b.dng");
    widget.setFailed("bad");
    CHECK(widget.toolTip() == "Render failed: bad");
    CHECK(widget.opening());
    widget.setOpening("c.dng");
    CHECK_FALSE(widget.failed());
    widget.setPhotoOpen(false);
    CHECK_FALSE(widget.opening());
    CHECK(widget.toolTip() == "No photograph open");
    CHECK(widget.stateColour() == theme::progressIdle);
}

TEST_CASE("The window's pie is not up to date until the first render is shown",
          "[app][window][progress]") {
    test::TempDir folder;
    const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
    std::filesystem::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
    DebugLog debugLog;
    MainWindow window(debugLog);
    window.show();
    auto& view = *window.findChild<PhotoView*>();
    auto& progress = *window.findChild<RenderProgressPie*>();
    window.openInitialPath(folder.file("a.dng"));
    // Opened, nothing decoded or shown yet.
    CHECK(view.wholeFrameImage().isNull());
    CHECK(progress.opening());
    CHECK(progress.toolTip() != "Up to date");
    CHECK(progress.toolTip().startsWith("Opening a.dng"));
    CHECK(progress.filled() < 1.0);

    REQUIRE(QTest::qWaitFor([&] { return !view.wholeFrameImage().isNull(); }, 20000));
    CHECK_FALSE(progress.opening());
    REQUIRE(QTest::qWaitFor([&] { return !progress.rendering(); }, 5000));
    CHECK(progress.toolTip() == "Up to date");
    CHECK(progress.filled() == 1.0);
}

TEST_CASE("The window's pie says when no photograph is open", "[app][window][progress]") {
    test::TempDir folder;
    const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
    std::filesystem::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
    DebugLog debugLog;
    MainWindow window(debugLog);
    window.show();
    auto& progress = *window.findChild<RenderProgressPie*>();
    CHECK_FALSE(progress.photoOpen());
    CHECK(progress.toolTip() == "No photograph open");

    window.openInitialPath(folder.file("a.dng"));
    CHECK(progress.photoOpen());
    CHECK(progress.toolTip() != "No photograph open");
}

TEST_CASE("The window's pie is always there, beside a status message", "[app][window][progress]") {
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
    auto& progress = *window.findChild<RenderProgressPie*>();
    // In the status bar, not over the photograph.
    CHECK(progress.parentWidget() != &view);
    CHECK(view.findChild<RenderProgressPie*>() == nullptr);
    // The first render was over in a moment.
    QTest::qWait(700);
    CHECK(progress.isVisible());
    CHECK(progress.filled() == 1.0);

    // A render that takes its time: the pie waits out the delay, then empties.
    RenderIndicator& indicator = window.renderIndicator();
    indicator.begin();
    CHECK_FALSE(progress.rendering());
    CHECK(progress.isVisible());
    CHECK(QTest::qWaitFor([&] { return progress.rendering(); }, 2000));
    CHECK(progress.toolTip() == QString::fromUtf8("Developing\u2026"));
    // Beside an export's message, not hidden by it.
    window.statusBar()->showMessage("Exporting a.dng...");
    CHECK(progress.isVisible());
    CHECK(window.statusBar()->currentMessage() == "Exporting a.dng...");
    window.statusBar()->clearMessage();

    indicator.report(0.4, ProgressStep::Denoise);
    CHECK(progress.display().fraction == 0.4);
    CHECK(progress.toolTip() == QString::fromUtf8("Reducing noise\u2026 40%"));

    // In the crop mode as well.
    view.setCropMode(true);
    CHECK(progress.isVisible());
    view.setCropMode(false);

    indicator.finish();
    CHECK(QTest::qWaitFor([&] { return !progress.rendering(); }, 2000));
    CHECK(progress.isVisible());
    CHECK(progress.filled() == 1.0);
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
    auto& progress = *window.findChild<RenderProgressPie*>();
    // The first render's image ended its busy period.
    REQUIRE(QTest::qWaitFor([&] { return !indicator.busy(); }, 20000));

    // A new size asks for a render, which begins one; its image ends it.
    window.resize(window.size() + QSize(40, 30));
    REQUIRE(QTest::qWaitFor([&] { return indicator.busy(); }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return !indicator.busy(); }, 20000));

    // Progress of a request that is not the newest does not reach the pie.
    indicator.begin();
    REQUIRE(QTest::qWaitFor([&] { return progress.rendering(); }, 2000));
    window.showRenderProgress(0, 0.5, ProgressStep::Denoise);
    CHECK_FALSE(progress.display().fraction.has_value());
    indicator.finish(false);
    CHECK(QTest::qWaitFor([&] { return !progress.rendering(); }, 2000));
}
