#include "ui/HistogramPanel.h"
#include "ui/HistogramView.h"
#include "ui/ThemeColors.h"

#include <OutputHistogram.h>

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The histogram panel: its plot, triangles, channel switch and readout (ADR 055, section 7).

namespace {

/// Forgets what the panel remembers, before and after a case, so cases do not meet.
struct CleanSettings {
    static void clear() {
        QSettings().remove("histogram");
    }

    CleanSettings() {
        clear();
    }

    ~CleanSettings() {
        clear();
    }
};

/// A histogram of 1000 pixels with the given channels clipped at the ends.
OutputHistogram clipped(bool shadowsRed, bool highlightsRed, bool highlightsGreen) {
    OutputHistogram histogram;
    histogram.pixels = 1000;
    for (std::size_t bin = 1; bin < 255; ++bin) {
        histogram.red[bin] = histogram.green[bin] = histogram.blue[bin] = histogram.luma[bin] = 3;
    }
    if (shadowsRed) {
        histogram.red[0] = 7;
        histogram.shadowsClipped = 7;
    }
    if (highlightsRed || highlightsGreen) {
        histogram.red[255] = highlightsRed ? 11 : 0;
        histogram.green[255] = highlightsGreen ? 4 : 0;
        histogram.highlightsClipped = 12;
    }
    return histogram;
}

QToolButton* button(const HistogramPanel& panel, const char* name) {
    auto* found = panel.findChild<QToolButton*>(name);
    REQUIRE(found != nullptr);
    return found;
}

/// A shown panel of a fixed width, so that the layout is settled.
void settle(HistogramPanel& panel) {
    panel.resize(320, 180);
    panel.show();
    QApplication::processEvents();
}

} // namespace

TEST_CASE("the channel switch is remembered in QSettings", "[app][histogram][panel]") {
    const CleanSettings clean;
    {
        HistogramPanel panel;
        CHECK(panel.channels() == HistogramChannels::Rgb);
        CHECK(button(panel, "channelRgb")->isChecked());
        button(panel, "channelLuma")->click();
        CHECK(panel.channels() == HistogramChannels::Luma);
        CHECK(button(panel, "channelLuma")->isChecked());
        CHECK_FALSE(button(panel, "channelRgb")->isChecked());
    }
    CHECK(QSettings().value("histogram/channels").toString() == "luma");
    {
        HistogramPanel panel;
        CHECK(panel.channels() == HistogramChannels::Luma);
        CHECK(panel.view()->channels() == HistogramChannels::Luma);
        button(panel, "channelRgb")->click();
        CHECK(panel.channels() == HistogramChannels::Rgb);
    }
    CHECK(QSettings().value("histogram/channels").toString() == "rgb");
    CHECK(HistogramPanel().channels() == HistogramChannels::Rgb);
}

TEST_CASE("triangles are unlit without clipping and lit with it, coloured by channel",
          "[app][histogram][panel]") {
    const CleanSettings clean;
    HistogramPanel panel;
    CHECK_FALSE(panel.triangleColour(ClipKind::Shadows));
    CHECK_FALSE(panel.triangleColour(ClipKind::Highlights));

    panel.showHistogram(clipped(false, false, false));
    CHECK_FALSE(panel.triangleColour(ClipKind::Shadows));
    CHECK_FALSE(panel.triangleColour(ClipKind::Highlights));

    panel.showHistogram(clipped(true, true, true));
    CHECK(panel.triangleColour(ClipKind::Shadows) == clipColour({true, false, false}));
    CHECK(panel.triangleColour(ClipKind::Highlights) == clipColour({true, true, false}));
    CHECK(clipColour({true, true, false}) != clipColour({true, false, false}));

    panel.showHistogram(clipped(false, false, true));
    CHECK_FALSE(panel.triangleColour(ClipKind::Shadows));
    CHECK(panel.triangleColour(ClipKind::Highlights) == clipColour({false, true, false}));

    panel.clear();
    CHECK_FALSE(panel.triangleColour(ClipKind::Highlights));
}

TEST_CASE("triangle tooltips give the clipped share", "[app][histogram][panel]") {
    const CleanSettings clean;
    HistogramPanel panel;
    panel.showHistogram(clipped(false, true, true));
    const QString highlights = button(panel, "highlightsTriangle")->toolTip();
    CHECK(highlights.contains("Highlights clipped: 1.2 % of pixels (R 1.1 %, G 0.4 %, B 0.0 %)"));
    CHECK(button(panel, "shadowsTriangle")->toolTip().contains("Shadows clipped: none"));
    CHECK(button(panel, "highlightsTriangle")->accessibleDescription() == highlights);
}

TEST_CASE("clicking a triangle emits clippingToggled", "[app][histogram][panel]") {
    const CleanSettings clean;
    HistogramPanel panel;
    panel.showHistogram(clipped(true, true, false));
    QSignalSpy spy(&panel, &HistogramPanel::clippingToggled);

    button(panel, "highlightsTriangle")->click();
    REQUIRE(spy.count() == 1);
    CHECK(spy.at(0).at(0).toBool() == false);
    CHECK(spy.at(0).at(1).toBool() == true);
    CHECK(panel.clippingShown(ClipKind::Highlights));

    button(panel, "shadowsTriangle")->click();
    REQUIRE(spy.count() == 2);
    CHECK(spy.at(1).at(0).toBool() == true);
    CHECK(spy.at(1).at(1).toBool() == true);

    button(panel, "highlightsTriangle")->click();
    REQUIRE(spy.count() == 3);
    CHECK(spy.at(2).at(0).toBool() == true);
    CHECK(spy.at(2).at(1).toBool() == false);

    // Setting the state from outside is not the user's toggle.
    panel.setClippingShown(false, true);
    CHECK(spy.count() == 3);
    CHECK_FALSE(panel.clippingShown(ClipKind::Shadows));
    CHECK(panel.clippingShown(ClipKind::Highlights));
}

TEST_CASE("triangles are disabled while there is no histogram", "[app][histogram][panel]") {
    const CleanSettings clean;
    HistogramPanel panel;
    CHECK_FALSE(button(panel, "shadowsTriangle")->isEnabled());
    CHECK_FALSE(button(panel, "highlightsTriangle")->isEnabled());

    panel.showHistogram(clipped(false, false, false));
    CHECK(button(panel, "shadowsTriangle")->isEnabled());
    CHECK(button(panel, "highlightsTriangle")->isEnabled());

    panel.clear();
    CHECK_FALSE(panel.hasHistogram());
    CHECK_FALSE(button(panel, "shadowsTriangle")->isEnabled());
}

TEST_CASE("the readout keeps its height with and without a colour", "[app][histogram][panel]") {
    const CleanSettings clean;
    HistogramPanel panel;
    settle(panel);
    auto* readout = panel.findChild<QLabel*>("histogramReadout");
    REQUIRE(readout != nullptr);
    const int dashed = readout->height();
    const int panelHeight = panel.height();
    CHECK(panel.readout() == readoutText(std::nullopt));

    panel.setPointerColour(qRgb(115, 102, 97));
    QApplication::processEvents();
    CHECK(panel.readout() == "R 45.1 %   G 40.0 %   B 38.0 %");
    CHECK(readout->height() == dashed);
    CHECK(panel.height() == panelHeight);

    panel.setPointerColour(std::nullopt);
    QApplication::processEvents();
    CHECK(panel.readout() == readoutText(std::nullopt));
    CHECK(readout->height() == dashed);
}

TEST_CASE("accessible names are set", "[app][histogram][panel]") {
    const CleanSettings clean;
    HistogramPanel panel;
    CHECK(panel.view()->accessibleName() == "Histogram");
    CHECK(panel.view()->accessibleDescription() == "No histogram");
    CHECK(button(panel, "shadowsTriangle")->accessibleName() == "Show clipped shadows");
    CHECK(button(panel, "highlightsTriangle")->accessibleName() == "Show clipped highlights");
    CHECK(button(panel, "channelRgb")->accessibleName() == "RGB");
    CHECK(button(panel, "channelLuma")->accessibleName() == "Luma");
    CHECK_FALSE(panel.findChild<QLabel*>("histogramReadout")->accessibleName().isEmpty());

    panel.showHistogram(clipped(true, true, false));
    const QString description = panel.view()->accessibleDescription();
    CHECK(description.contains("Shadows clipped"));
    CHECK(description.contains("Highlights clipped"));
}

TEST_CASE("no button takes the focus on a click", "[app][histogram][panel]") {
    const CleanSettings clean;
    HistogramPanel panel;
    for (const char* name :
         {"shadowsTriangle", "highlightsTriangle", "channelRgb", "channelLuma"}) {
        INFO(name);
        const Qt::FocusPolicy policy = button(panel, name)->focusPolicy();
        CHECK((policy & Qt::TabFocus) != 0);
        CHECK((policy & Qt::ClickFocus) == 0);
    }
    CHECK(panel.view()->focusPolicy() == Qt::NoFocus);
}

TEST_CASE("painting a histogram draws end markers outside the plot area",
          "[app][histogram][panel]") {
    const CleanSettings clean;
    HistogramPanel panel;
    settle(panel);
    HistogramView* view = panel.view();

    // Only the end bins: the markers are drawn on their own scale.
    OutputHistogram ends;
    ends.pixels = 300;
    ends.red[255] = 100;
    ends.blue[0] = 50;
    ends.red[0] = 0;
    panel.showHistogram(ends);
    QImage picture = view->grab().toImage();
    REQUIRE(picture.size() == view->size() * picture.devicePixelRatio());

    const QRectF plot = view->plotRect();
    const QRectF right = view->markerRect(true);
    const QRectF left = view->markerRect(false);
    CHECK(right.left() >= plot.right());
    CHECK(left.right() <= plot.left());
    const int y = static_cast<int>(plot.bottom()) - 3;
    // Red is the first of three columns in the right gutter, blue the last of the left one.
    CHECK(picture.pixelColor(static_cast<int>(right.left()) + 1, y) == theme::histogramRed);
    CHECK(picture.pixelColor(static_cast<int>(left.left()) + 3, y) == theme::histogramBlue);
    // Above the half-height blue marker the gutter is the ground; so is the plot.
    CHECK(picture.pixelColor(static_cast<int>(left.left()) + 3, static_cast<int>(plot.top()) + 6) ==
          theme::histogramGround);
    CHECK(picture.pixelColor(static_cast<int>(plot.center().x()), y) == theme::histogramGround);

    // An empty histogram draws no marker.
    panel.clear();
    picture = view->grab().toImage();
    CHECK(picture.pixelColor(static_cast<int>(right.left()) + 1, y) == theme::histogramGround);
}

TEST_CASE("RGB channels add where they overlap", "[app][histogram][panel]") {
    const CleanSettings clean;
    HistogramPanel panel;
    settle(panel);
    HistogramView* view = panel.view();

    OutputHistogram histogram;
    histogram.pixels = 1000;
    for (std::size_t bin = 100; bin < 140; ++bin) {
        histogram.red[bin] = 50;
        histogram.green[bin] = 50;
    }
    for (std::size_t bin = 140; bin < 180; ++bin) {
        histogram.blue[bin] = 50;
    }
    panel.showHistogram(histogram);
    const QImage picture = view->grab().toImage();
    const QRectF plot = view->plotRect();
    const qreal binWidth = plot.width() / 254.0;
    const auto at = [&](std::size_t bin) {
        const int x = static_cast<int>(plot.left() + (static_cast<qreal>(bin) - 0.5) * binWidth);
        return picture.pixelColor(x, static_cast<int>(plot.bottom()) - 4);
    };
    const QColor both = at(120);
    CHECK(both.red() > 200);
    CHECK(both.green() > 200);
    CHECK(both.blue() < 150);
    const QColor blue = at(160);
    CHECK(blue.blue() > 200);
    CHECK(blue.red() < 100);
    CHECK(at(30) == theme::histogramGround);
}
