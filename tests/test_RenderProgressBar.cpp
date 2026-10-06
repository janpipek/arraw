#include "support/TempDir.h"
#include "ui/CropOverlay.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/RenderProgressBar.h"

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QPalette>
#include <QStatusBar>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace arraw;
using namespace arraw::app;

/// The bar along the top of the photo view and the step in the status bar (ADR 042).

namespace {

RenderActivity::Display displayOf(std::optional<double> fraction, double sweep = 0.0) {
    return {.visible = true, .fraction = fraction, .step = ProgressStep::Denoise, .sweep = sweep};
}

} // namespace

TEST_CASE("The bar fills a fraction of the track in whole device pixels", "[app][progress][bar]") {
    CHECK(RenderProgressBar::fill(displayOf(0.4), 1000) == std::pair{0, 400});
    CHECK(RenderProgressBar::fill(displayOf(0.0), 1000).second == 0);
    CHECK(RenderProgressBar::fill(displayOf(1.0), 333) == std::pair{0, 333});
    CHECK(RenderProgressBar::fill(RenderActivity::Display{}, 1000).second == 0);
}

TEST_CASE("The sweep enters, crosses and leaves the track", "[app][progress][bar]") {
    const int width = 1000;
    const auto [enterLeft, enterLength] = RenderProgressBar::fill(displayOf({}, 0.0), width);
    CHECK(enterLength == 0); // Entirely outside, to the left.
    const auto [midLeft, midLength] = RenderProgressBar::fill(displayOf({}, 0.5), width);
    CHECK(midLength == 300);
    CHECK(midLeft > 0);
    CHECK(midLeft + midLength < width);
    const auto [endLeft, endLength] = RenderProgressBar::fill(displayOf({}, 0.99), width);
    CHECK(endLeft + endLength <= width);
}

TEST_CASE("The bar is hidden until shown, takes no mouse events and paints the highlight",
          "[app][progress][bar]") {
    PhotoView view;
    view.resize(200, 100);
    view.show();
    RenderProgressBar& bar = view.progressBar();
    CHECK_FALSE(bar.isVisible());
    CHECK(bar.testAttribute(Qt::WA_TransparentForMouseEvents));
    CHECK(bar.height() == RenderProgressBar::thickness);
    CHECK(bar.width() == view.width());

    bar.setDisplay(displayOf(0.5));
    REQUIRE(bar.isVisible());
    const QImage grabbed = view.grab().toImage();
    const QColor highlight = QApplication::palette().color(QPalette::Highlight);
    CHECK(grabbed.pixelColor(10, 1) == highlight);
    // Past the fill is the track, which is not the highlight itself.
    CHECK(grabbed.pixelColor(190, 1) != highlight);
    // Below the bar is the view.
    CHECK(grabbed.pixelColor(10, RenderProgressBar::thickness + 2) != highlight);

    bar.setDisplay({});
    CHECK_FALSE(bar.isVisible());
}

TEST_CASE("The bar lies over the crop overlay and follows the view's width",
          "[app][progress][bar]") {
    PhotoView view;
    view.resize(200, 100);
    view.show();
    view.progressBar().setDisplay(displayOf(0.5));
    view.setCropMode(true);
    CHECK(view.progressBar().isVisible());
    const auto children = view.children();
    CHECK(children.indexOf(&view.progressBar()) >= 0);
    view.resize(300, 100);
    CHECK(view.progressBar().width() == 300);
    view.setCropMode(false);
    CHECK(view.progressBar().isVisible());
}

TEST_CASE("The window shows a long render's step and bar, and hides them when it is done",
          "[app][window][progress]") {
    test::TempDir folder;
    const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
    std::filesystem::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
    MainWindow window;
    window.openInitialPath(folder.file("a.dng"));
    window.show();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    auto& view = *window.findChild<PhotoView*>();
    REQUIRE(QTest::qWaitFor([&] { return !view.wholeFrameImage().isNull(); }, 20000));
    auto* label = window.findChild<QLabel*>("renderStepLabel");
    REQUIRE(label != nullptr);
    // The first render was over in a moment.
    QTest::qWait(700);
    CHECK_FALSE(view.progressBar().isVisible());
    CHECK_FALSE(label->isVisible());

    // A render that takes its time: the bar waits out the delay, then shows.
    RenderIndicator& indicator = window.renderIndicator();
    indicator.begin();
    CHECK_FALSE(view.progressBar().isVisible());
    CHECK(QTest::qWaitFor([&] { return view.progressBar().isVisible(); }, 2000));
    CHECK(label->isVisible());
    CHECK(label->text() == QString::fromUtf8("Developing…"));
    // Beside an export's message, not in its place.
    window.statusBar()->showMessage("Exporting a.dng…");
    CHECK(label->isVisible());
    window.statusBar()->clearMessage();

    indicator.report(0.4, ProgressStep::Denoise);
    CHECK(view.progressBar().display().fraction == 0.4);
    CHECK(label->text() == QString::fromUtf8("Reducing noise…"));

    // In the crop mode as well.
    view.setCropMode(true);
    CHECK(view.progressBar().isVisible());
    view.setCropMode(false);

    indicator.finish();
    CHECK(QTest::qWaitFor([&] { return !view.progressBar().isVisible(); }, 2000));
    CHECK_FALSE(label->isVisible());
}

TEST_CASE("The window's renders begin and end the busy period, and stale progress is ignored",
          "[app][window][progress]") {
    test::TempDir folder;
    const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
    std::filesystem::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
    MainWindow window;
    window.openInitialPath(folder.file("a.dng"));
    window.show();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    auto& view = *window.findChild<PhotoView*>();
    REQUIRE(QTest::qWaitFor([&] { return !view.wholeFrameImage().isNull(); }, 20000));
    RenderIndicator& indicator = window.renderIndicator();
    // The first render's image ended its busy period.
    REQUIRE(QTest::qWaitFor([&] { return !indicator.busy(); }, 20000));

    // A new size asks for a render, which begins one; its image ends it.
    window.resize(window.size() + QSize(40, 30));
    REQUIRE(QTest::qWaitFor([&] { return indicator.busy(); }, 5000));
    REQUIRE(QTest::qWaitFor([&] { return !indicator.busy(); }, 20000));

    // Progress of a request that is not the newest does not reach the bar.
    indicator.begin();
    REQUIRE(QTest::qWaitFor([&] { return view.progressBar().isVisible(); }, 2000));
    window.showRenderProgress(0, 0.5, ProgressStep::Denoise);
    CHECK_FALSE(view.progressBar().display().fraction.has_value());
    indicator.finish(false);
    CHECK(QTest::qWaitFor([&] { return !view.progressBar().isVisible(); }, 2000));
}
