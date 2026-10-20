#include "CropSpans.h"
#include "DebugLog.h"
#include "HistogramCounter.h"
#include "HistogramPresentation.h"
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

#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QTest>

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

TEST_CASE("Zooming to 1:1 and panning leave the histogram unchanged", "[app][window][histogram]") {
    Window w;
    const OutputHistogram before = *w.shown();
    const std::uint64_t submits = w.window.histogramSubmitCount();

    w.view().zoomTo(4.0);
    w.view().panBy({20.0, 10.0});
    w.view().panBy({-5.0, 3.0});
    QTest::qWait(600);
    // Whatever refreshed beneath, the count is of the whole frame, and the same pixels.
    REQUIRE(w.showsWholeFrame());
    CHECK(w.shown()->pixels == before.pixels);
    CHECK(w.window.histogramSubmitCount() >= submits);
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
    w.view().zoomTo(4.0);
    QTest::qWait(300);
    const OutputHistogram before = *w.shown();
    w.exposureBox().setValue(-2.0);
    w.develop().finishPendingEdit();
    REQUIRE(QTest::qWaitFor([&] { return w.shown() && *w.shown() != before; }, 10000));
    w.settle();
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
