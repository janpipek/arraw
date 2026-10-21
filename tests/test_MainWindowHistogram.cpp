#include "CropSpans.h"
#include "DebugLog.h"
#include "HistogramCounter.h"
#include "HistogramPresentation.h"
#include "ViewTransform.h"
#include "support/TempDir.h"
#include "ui/CollapsibleSection.h"
#include "ui/CropOverlay.h"
#include "ui/DevelopPanel.h"
#include "ui/FilmStrip.h"
#include "ui/HistogramPanel.h"
#include "ui/HistogramView.h"
#include "ui/MainWindow.h"
#include "ui/PhotoView.h"
#include "ui/SettingSlider.h"

#include <OutputHistogram.h>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <optional>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The histogram panel inside the window: placement, feeding, gating and the readout
/// (ADR 055, sections 3 and 6).

namespace {

/// Forgets what the section and the panel remember, before and after a case.
struct CleanSettings {
    static void clear() {
        QSettings settings;
        settings.remove(CollapsibleSection::settingsKey("histogram"));
        settings.remove("histogram");
    }

    CleanSettings() {
        clear();
    }

    ~CleanSettings() {
        clear();
    }
};

/// Counts a picture as the counter thread would.
OutputHistogram counted(const QImage& image, const std::vector<RowSpan>& spans = {}) {
    const std::atomic<bool> never{false};
    return countImageCodes(image, spans, NamedEncoding::Srgb, never);
}

/// Tells whether a render region leaves out part of the frame.
bool isRegion(const QRectF& region) {
    return !region.isEmpty() && (region.width() < 0.99 || region.height() < 0.99);
}

/// A window with a folder of two photographs open, the first one shown and rendered.
struct Window {
    CleanSettings clean;
    test::TempDir folder;
    DebugLog debugLog;
    MainWindow window{debugLog};

    Window() {
        const std::filesystem::path fixtures(ARRAW_TEST_DATA_DIR);
        std::filesystem::copy_file(fixtures / "preview-32x24.dng", folder.file("a.dng"));
        std::filesystem::copy_file(fixtures / "bayer-32x24.dng", folder.file("b.dng"));
        window.resize(1400, 500);
        window.openInitialPath(folder.file("a.dng"));
        window.show();
        window.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&window));
        REQUIRE(QTest::qWaitFor([this] { return !view().wholeFrameImage().isNull(); }, 20000));
        REQUIRE(QTest::qWaitFor([this] { return !strip().isListing(); }, 10000));
        settle();
    }

    [[nodiscard]] PhotoView& view() const {
        return *window.findChild<PhotoView*>();
    }

    [[nodiscard]] CropOverlay& overlay() const {
        return *window.findChild<CropOverlay*>();
    }

    [[nodiscard]] FilmStrip& strip() const {
        return *window.findChild<FilmStrip*>();
    }

    [[nodiscard]] DevelopPanel& develop() const {
        return *window.findChild<DevelopPanel*>();
    }

    [[nodiscard]] HistogramPanel& panel() const {
        return *window.findChild<HistogramPanel*>();
    }

    [[nodiscard]] QDockWidget& dock() const {
        return *window.findChild<QDockWidget*>("DevelopDock");
    }

    [[nodiscard]] CollapsibleSection& section() const {
        for (auto* found : window.findChildren<CollapsibleSection*>()) {
            if (found->id() == "histogram") {
                return *found;
            }
        }
        FAIL("no histogram section");
        throw;
    }

    [[nodiscard]] QDoubleSpinBox& exposureBox() const {
        for (auto* row : window.findChildren<SettingSlider*>()) {
            if (row->key() == "exposure") {
                return *row->findChild<QDoubleSpinBox*>();
            }
        }
        FAIL("no exposure row");
        throw;
    }

    /// What the panel shows; empty before the first count.
    [[nodiscard]] std::optional<OutputHistogram> shown() const {
        return panel().view()->histogram();
    }

    /// Waits until the panel shows the count of the whole-frame image of the photo view.
    [[nodiscard]] bool showsWholeFrame() const {
        return QTest::qWaitFor(
            [this] {
                const auto now = shown();
                return now && *now == counted(view().wholeFrameImage());
            },
            10000);
    }

    /// Lets the renders and recounts that follow the last action land.
    void settle() const {
        REQUIRE(showsWholeFrame());
        QTest::qWait(400);
        REQUIRE(showsWholeFrame());
    }
};

} // namespace

TEST_CASE("The histogram section sits above the tabs and remembers being closed",
          "[app][window][histogram]") {
    Window w;
    QWidget* tabs = w.window.findChild<QWidget*>("developTabs");
    REQUIRE(tabs != nullptr);
    CHECK(w.section().mapTo(&w.dock(), QPoint()).y() < tabs->mapTo(&w.dock(), QPoint()).y());
    CHECK(w.section().isOpen());
    CHECK(w.section().isAncestorOf(&w.panel()));

    w.section().setOpen(false);
    CHECK_FALSE(w.panel().isVisible());
    {
        DebugLog log;
        MainWindow again(log);
        for (auto* found : again.findChildren<CollapsibleSection*>()) {
            if (found->id() == "histogram") {
                CHECK_FALSE(found->isOpen());
            }
        }
    }
}

TEST_CASE("Opening a photograph shows the histogram of the shown picture",
          "[app][window][histogram]") {
    Window w;
    const auto now = w.shown();
    REQUIRE(now.has_value());
    CHECK(now->pixels > 0);
    CHECK(*now == counted(w.view().wholeFrameImage()));
    CHECK(w.panel().hasHistogram());
}

TEST_CASE("Zooming in and panning leave the histogram unchanged", "[app][window][histogram]") {
    Window w;
    const OutputHistogram before = *w.shown();

    // The fixture is 32 x 24: at the largest zoom its height overflows the view, so the render
    // is a region and the whole-frame picture is the fallback beneath it.
    w.view().zoomTo(maxZoom);
    REQUIRE(w.view().devicePixels().height() < 24 * maxZoom);
    REQUIRE(QTest::qWaitFor([&] { return isRegion(w.view().renderRegion()); }, 20000));
    w.view().panBy({20.0, 10.0});
    w.view().panBy({-5.0, 3.0});
    QTest::qWait(600);
    REQUIRE(isRegion(w.view().renderRegion()));
    // Whatever refreshed beneath, the count is of the whole frame, and the same pixels.
    REQUIRE(w.showsWholeFrame());
    CHECK(w.shown()->pixels == before.pixels);
    CHECK(*w.shown() == counted(w.view().wholeFrameImage()));
}

TEST_CASE("An exposure edit changes the histogram without waiting for a pause",
          "[app][window][histogram]") {
    Window w;
    const OutputHistogram before = *w.shown();
    w.exposureBox().setValue(2.0);
    REQUIRE(QTest::qWaitFor([&] { return w.shown() && *w.shown() != before; }, 10000));
    w.develop().finishPendingEdit();
    w.settle();
    CHECK(*w.shown() != before);
}

TEST_CASE("While zoomed in, the histogram follows the refreshed fallback",
          "[app][window][histogram]") {
    Window w;
    w.view().zoomTo(maxZoom);
    REQUIRE(QTest::qWaitFor([&] { return isRegion(w.view().renderRegion()); }, 20000));
    QTest::qWait(300);
    REQUIRE(isRegion(w.view().renderRegion()));
    const OutputHistogram before = *w.shown();
    w.exposureBox().setValue(-2.0);
    w.develop().finishPendingEdit();
    REQUIRE(QTest::qWaitFor([&] { return w.shown() && *w.shown() != before; }, 10000));
    w.settle();
    REQUIRE(isRegion(w.view().renderRegion()));
    CHECK(*w.shown() == counted(w.view().wholeFrameImage()));
}

TEST_CASE("A crop drag in the crop mode changes the histogram without a render",
          "[app][window][histogram][crop]") {
    Window w;
    QTest::keyClick(&w.window, Qt::Key_C);
    REQUIRE(w.view().isCropMode());
    REQUIRE(QTest::qWaitFor([&] { return w.overlay().hasRender(); }, 20000));

    // What the mode counts: its own render, through the crop frame, however it is turned.
    const auto expected = [&] {
        const QImage image = w.overlay().image();
        return counted(image, cropSpans(image.size(), w.overlay().cropQuadInImage()));
    };
    REQUIRE(QTest::qWaitFor([&] { return w.shown() && *w.shown() == expected(); }, 10000));
    QTest::qWait(300);
    REQUIRE(*w.shown() == expected());
    const OutputHistogram before = *w.shown();

    w.overlay().edit([](CropEditing& editing) {
        const CropBox crop = editing.crop();
        editing.resizeTo(CropHandle::BottomRight,
                         {crop.left + crop.width / 2, crop.top + crop.height / 2});
    });
    REQUIRE(QTest::qWaitFor([&] { return w.shown() && *w.shown() == expected(); }, 10000));
    CHECK(w.shown()->pixels < before.pixels);

    // A turn of the picture under the frame counts the pixels the rotated frame covers.
    w.overlay().edit([](CropEditing& editing) { editing.rotateTo(-9.0); });
    REQUIRE(QTest::qWaitFor([&] { return w.shown() && *w.shown() == expected(); }, 10000));
    CHECK(w.shown()->pixels > 0);
}

TEST_CASE("The mask tint is not counted", "[app][window][histogram][masks]") {
    Window w;
    QTest::keyClick(&w.window, Qt::Key_M);
    REQUIRE(w.view().isMaskMode());
    w.settle();
    CHECK(*w.shown() == counted(w.view().wholeFrameImage()));
}

TEST_CASE("Nothing is counted while the section is closed or the dock hidden",
          "[app][window][histogram]") {
    Window w;
    SECTION("closed section") {
        w.section().setOpen(false);
        const std::uint64_t submits = w.window.histogramSubmitCount();
        w.exposureBox().setValue(1.5);
        w.develop().finishPendingEdit();
        QTest::qWait(700);
        CHECK(w.window.histogramSubmitCount() == submits);
        // What it showed is gone, not left to go stale.
        CHECK_FALSE(w.panel().hasHistogram());

        w.section().setOpen(true);
        CHECK(w.window.histogramSubmitCount() > submits);
        w.settle();
    }
    SECTION("hidden dock") {
        w.dock().hide();
        const std::uint64_t submits = w.window.histogramSubmitCount();
        w.view().zoomTo(3.0);
        w.view().zoomToFit();
        QTest::qWait(700);
        CHECK(w.window.histogramSubmitCount() == submits);
        CHECK_FALSE(w.panel().hasHistogram());

        w.dock().show();
        CHECK(w.window.histogramSubmitCount() > submits);
        w.settle();
    }
}

TEST_CASE("Opening another photograph clears the histogram", "[app][window][histogram]") {
    Window w;
    REQUIRE(w.panel().hasHistogram());
    w.window.openInitialPath(w.folder.file("b.dng"));
    CHECK_FALSE(w.panel().hasHistogram());
    REQUIRE(QTest::qWaitFor([&] { return w.panel().hasHistogram(); }, 20000));
    w.settle();
}

TEST_CASE("The readout shows the pixel under the pointer and dashes over the letterbox",
          "[app][window][histogram]") {
    Window w;
    PhotoView& view = w.view();
    const QSizeF frame(view.size());
    const ViewTransform transform = view.transform();
    const double ratio = view.devicePixelRatioF();
    const auto position = [&](QPointF fraction) {
        return (transform.viewFromFrame(fraction) / ratio).toPoint();
    };

    // The window is wide: the frame is fitted by its height, with room at both sides.
    const QPoint outside = position({-0.2, 0.5});
    REQUIRE(QRectF(QPointF(), frame).contains(outside));
    QTest::mouseMove(&view, outside);
    REQUIRE(
        QTest::qWaitFor([&] { return w.panel().readout() == readoutText(std::nullopt); }, 3000));

    const QImage shown = view.wholeFrameImage();
    const QPoint inside = position({0.5, 0.5});
    QTest::mouseMove(&view, inside);
    REQUIRE(
        QTest::qWaitFor([&] { return w.panel().readout() != readoutText(std::nullopt); }, 3000));
    const QPointF fraction = transform.frameFromView(QPointF(inside) * ratio);
    const QImage image = view.wholeFrameImage();
    const QRgb colour = image.pixel(static_cast<int>(fraction.x() * image.width()),
                                    static_cast<int>(fraction.y() * image.height()));
    CHECK(w.panel().readout() == readoutText(colour));
}

namespace {

/// The View menu's Show Clipping action of a window.
QAction& clippingAction(MainWindow& window) {
    for (QAction* candidate : window.findChildren<QAction*>()) {
        if (candidate->objectName() == "showClippingAction") {
            return *candidate;
        }
    }
    FAIL("no Show Clipping action");
    throw;
}

/// A bare view of a 4x4 frame at 1:1 showing a light picture whose pixels all clip highlights.
struct ClippedView {
    PhotoView view;
    QImage picture{4, 4, QImage::Format_RGBA8888};

    ClippedView() {
        picture.fill(QColor(255, 128, 128));
        view.resize(8, 8);
        view.setFrameSize(picture.size());
        view.zoomTo(1.0);
        view.show();
        REQUIRE(QTest::qWaitForWindowExposed(&view));
        view.setImage(picture, QRectF(0, 0, 1, 1), picture);
    }

    [[nodiscard]] QColor centre() {
        return QColor(view.grab().toImage().pixel(4, 4));
    }
};

} // namespace

TEST_CASE("With highlights shown, clipped pixels are painted red at 1:1", "[app][view][clipping]") {
    ClippedView v;
    CHECK(v.centre() == QColor(255, 128, 128));
    v.view.setClippingShown(false, true);
    const QColor painted = v.centre();
    CHECK(painted.red() > 200);
    CHECK(painted.green() < 60);
    CHECK(painted.blue() < 60);
    v.view.setClippingShown(true, false);
    CHECK(v.centre() == QColor(255, 128, 128));
}

TEST_CASE("The clipping overlay follows a new render", "[app][view][clipping]") {
    ClippedView v;
    v.view.setClippingShown(true, true);
    CHECK(v.centre().green() < 60);
    QImage dark(4, 4, QImage::Format_RGBA8888);
    dark.fill(QColor(0, 0, 0));
    v.view.setImage(dark, QRectF(0, 0, 1, 1), dark);
    const QColor painted = v.centre();
    CHECK(painted.blue() > 200);
    CHECK(painted.red() < 60);
    QImage grey(4, 4, QImage::Format_RGBA8888);
    grey.fill(QColor(100, 100, 100));
    v.view.setImage(grey, QRectF(0, 0, 1, 1), grey);
    CHECK(v.centre() == QColor(100, 100, 100));
}

TEST_CASE("No clipping overlay in the crop mode", "[app][view][clipping]") {
    ClippedView v;
    v.view.setClippingShown(true, true);
    REQUIRE(v.centre().green() < 60);
    v.view.setCropMode(true);
    // The crop overlay has no image, so it paints nothing of its own over the middle.
    CHECK(v.centre().green() >= 60);
}

TEST_CASE("J turns both on, then both off", "[app][window][histogram][clipping]") {
    Window w;
    QAction& action = clippingAction(w.window);
    CHECK_FALSE(action.isChecked());
    QTest::keyClick(&w.window, Qt::Key_J);
    CHECK(w.panel().clippingShown(ClipKind::Shadows));
    CHECK(w.panel().clippingShown(ClipKind::Highlights));
    CHECK(action.isChecked());
    QTest::keyClick(&w.window, Qt::Key_J);
    CHECK_FALSE(w.panel().clippingShown(ClipKind::Shadows));
    CHECK_FALSE(w.panel().clippingShown(ClipKind::Highlights));
    CHECK_FALSE(action.isChecked());
}

TEST_CASE("J works with the panels hidden", "[app][window][histogram][clipping]") {
    Window w;
    QTest::keyClick(&w.window, Qt::Key_F12);
    REQUIRE(w.dock().isHidden());
    QTest::keyClick(&w.window, Qt::Key_J);
    CHECK(w.panel().clippingShown(ClipKind::Highlights));
    QTest::keyClick(&w.window, Qt::Key_F12);
}

TEST_CASE("A triangle click and J agree", "[app][window][histogram][clipping]") {
    Window w;
    auto* highlights = w.panel().findChild<QToolButton*>("highlightsTriangle");
    REQUIRE(highlights != nullptr);
    highlights->click();
    CHECK(clippingAction(w.window).isChecked());
    CHECK(w.panel().clippingShown(ClipKind::Highlights));
    CHECK_FALSE(w.panel().clippingShown(ClipKind::Shadows));
    // One kind on, so J turns both on.
    QTest::keyClick(&w.window, Qt::Key_J);
    CHECK(w.panel().clippingShown(ClipKind::Shadows));
    CHECK(w.panel().clippingShown(ClipKind::Highlights));
    auto* shadows = w.panel().findChild<QToolButton*>("shadowsTriangle");
    REQUIRE(shadows != nullptr);
    shadows->click();
    CHECK_FALSE(w.panel().clippingShown(ClipKind::Shadows));
    CHECK(w.panel().clippingShown(ClipKind::Highlights));
}

TEST_CASE("The clipping overlay is remembered in QSettings", "[app][window][histogram][clipping]") {
    CleanSettings clean;
    {
        DebugLog debugLog;
        MainWindow window(debugLog);
        CHECK_FALSE(clippingAction(window).isChecked());
        clippingAction(window).trigger();
        CHECK(QSettings().value("histogram/clipping").toInt() == 3);
    }
    {
        DebugLog debugLog;
        MainWindow window(debugLog);
        CHECK(clippingAction(window).isChecked());
        auto* panel = window.findChild<HistogramPanel*>();
        REQUIRE(panel != nullptr);
        CHECK(panel->clippingShown(ClipKind::Shadows));
        CHECK(panel->clippingShown(ClipKind::Highlights));
        clippingAction(window).trigger();
    }
    QSettings settings;
    CHECK(settings.value("histogram/clipping").toInt() == 0);
    settings.setValue("histogram/clipping", 2);
    DebugLog debugLog;
    MainWindow window(debugLog);
    auto* panel = window.findChild<HistogramPanel*>();
    CHECK_FALSE(panel->clippingShown(ClipKind::Shadows));
    CHECK(panel->clippingShown(ClipKind::Highlights));
}

namespace {

/// A bare view of a 400 x 300 frame zoomed in to 1:1 on a 100 x 100 view, so that it shows
/// frame pixels 150..250 by 100..200, as a region render over a reduced fallback.
struct ZoomedView {
    PhotoView view;
    /// The fallback: a reduced whole frame, every pixel clipping highlights.
    QImage fallback{40, 30, QImage::Format_RGBA8888};
    /// The visible part of the frame, in fractions.
    QRectF visible{150.0 / 400.0, 100.0 / 300.0, 100.0 / 400.0, 100.0 / 300.0};

    ZoomedView() {
        fallback.fill(QColor(255, 128, 128));
        view.resize(100, 100);
        view.setFrameSize(QSize(400, 300));
        view.show();
        REQUIRE(QTest::qWaitForWindowExposed(&view));
        view.zoomTo(1.0);
        REQUIRE(view.zoom() == 1.0);
    }

    /// A render of the visible part, flat in one colour.
    [[nodiscard]] static QImage flat(QSize size, QColor colour) {
        QImage image(size, QImage::Format_RGBA8888);
        image.fill(colour);
        return image;
    }

    /// Counts the pixels of the view that look painted by an overlay (strongly red or blue).
    [[nodiscard]] int marked() {
        const QImage grab = view.grab().toImage();
        int count = 0;
        for (int y = 0; y < grab.height(); ++y) {
            for (int x = 0; x < grab.width(); ++x) {
                const QColor c(grab.pixel(x, y));
                if ((c.red() > 200 && c.green() < 60) || (c.blue() > 200 && c.red() < 60)) {
                    ++count;
                }
            }
        }
        return count;
    }
};

} // namespace

TEST_CASE("Zoomed in, the fallback's clipping never shows through the render",
          "[app][view][clipping]") {
    ZoomedView v;
    v.view.setImage(ZoomedView::flat({100, 100}, QColor(100, 100, 100)), v.visible, v.fallback);
    REQUIRE(v.view.wholeFrameImage().cacheKey() == v.fallback.cacheKey());
    v.view.setClippingShown(true, true);
    CHECK(v.marked() == 0);
    CHECK(QColor(v.view.grab().toImage().pixel(50, 50)) == QColor(100, 100, 100));
}

TEST_CASE("Zoomed in, the render's own clipping lines up with its pixels",
          "[app][view][clipping]") {
    ZoomedView v;
    QImage render = ZoomedView::flat({100, 100}, QColor(100, 100, 100));
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 50; ++x) {
            render.setPixelColor(x, y, QColor(255, 128, 128));
        }
    }
    v.view.setImage(render, v.visible, v.fallback);
    v.view.setClippingShown(false, true);
    const QImage grab = v.view.grab().toImage();
    CHECK(QColor(grab.pixel(25, 50)).red() > 200);
    CHECK(QColor(grab.pixel(25, 50)).green() < 60);
    CHECK(QColor(grab.pixel(75, 50)) == QColor(100, 100, 100));
}

TEST_CASE("Zoomed in, a render that no longer clips drops the marks of the one before",
          "[app][view][clipping]") {
    // A drag at 1:1: the fallback is refreshed only at the pause, so for a while it is of an
    // older state than the render.
    ZoomedView v;
    v.view.setClippingShown(true, true);
    v.view.setImage(ZoomedView::flat({100, 100}, QColor(255, 128, 128)), v.visible, v.fallback);
    CHECK(v.marked() > 9000);
    v.view.setImage(ZoomedView::flat({100, 100}, QColor(100, 100, 100)), v.visible, v.fallback);
    CHECK(v.marked() == 0);
    // The fallback catching up changes nothing of the render's marks.
    v.view.setBackground(ZoomedView::flat({40, 30}, QColor(255, 128, 128)));
    CHECK(v.marked() == 0);
}

TEST_CASE("A render smaller than the view is masked over its own rectangle only",
          "[app][view][clipping]") {
    ZoomedView v;
    // The render covers the middle 40 x 40 of what is visible; the fallback shows around it.
    const QRectF middle(190.0 / 400.0, 130.0 / 300.0, 40.0 / 400.0, 40.0 / 300.0);
    v.view.setImage(ZoomedView::flat({40, 40}, QColor(255, 128, 128)), middle, v.fallback);
    v.view.setClippingShown(false, true);
    const int marked = v.marked();
    CHECK(marked > 1400);
    CHECK(marked <= 40 * 40);
}

TEST_CASE("Zoomed in, the readout reads the render under the pointer, and follows changes under a "
          "still pointer",
          "[app][view][readout]") {
    ZoomedView v;
    QImage render = ZoomedView::flat({100, 100}, QColor(100, 100, 100));
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 50; ++x) {
            render.setPixelColor(x, y, QColor(10, 20, 30));
        }
    }
    v.view.setImage(render, v.visible, v.fallback);
    QSignalSpy spy(&v.view, &PhotoView::pointerColourChanged);
    const auto last = [&] { return spy.last().at(0).value<std::optional<QRgb>>(); };

    QTest::mouseMove(&v.view, QPoint(25, 50));
    REQUIRE(spy.count() >= 1);
    CHECK(last() == std::optional<QRgb>(QColor(10, 20, 30).rgb()));
    QTest::mouseMove(&v.view, QPoint(75, 50));
    CHECK(last() == std::optional<QRgb>(QColor(100, 100, 100).rgb()));

    // The pointer stays still while a new render lands, then while the picture is panned.
    v.view.setImage(ZoomedView::flat({100, 100}, QColor(1, 2, 3)), v.visible, v.fallback);
    CHECK(last() == std::optional<QRgb>(QColor(1, 2, 3).rgb()));
    // Panned by half the view, the render covers only part of it: the fallback is beneath.
    v.view.panBy({-60.0, 0.0});
    REQUIRE(last().has_value());
    CHECK(last() != std::optional<QRgb>(QColor(1, 2, 3).rgb()));
}
