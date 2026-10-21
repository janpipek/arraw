#include "HistogramPresentation.h"

#include <OutputHistogram.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

using namespace arraw;
using namespace arraw::app;

/// The numbers and words behind the histogram panel (ADR 055, sections 4, 6 and 7).

TEST_CASE("spikes in bins 0 and 255 do not set the scale", "[app][histogram][presentation]") {
    OutputHistogram histogram;
    for (std::size_t bin = 1; bin < 255; ++bin) {
        histogram.red[bin] = 10;
        histogram.luma[bin] = 10;
    }
    histogram.red[0] = 1'000'000;
    histogram.red[255] = 2'000'000;
    histogram.luma[0] = 1'000'000;
    histogram.luma[255] = 2'000'000;
    CHECK(histogramScale(histogram, HistogramChannels::Rgb) == 10);
    CHECK(histogramScale(histogram, HistogramChannels::Luma) == 10);
}

TEST_CASE("one interior spike is cut at the third tallest bin", "[app][histogram][presentation]") {
    OutputHistogram histogram;
    for (std::size_t bin = 1; bin < 255; ++bin) {
        histogram.green[bin] = 5;
    }
    histogram.green[100] = 900;
    histogram.green[101] = 800;
    histogram.green[102] = 70;
    histogram.green[103] = 60;
    CHECK(histogramScale(histogram, HistogramChannels::Rgb) == 70);

    // With a single spike, the third tallest is an ordinary bin.
    histogram.green[101] = 5;
    histogram.green[102] = 5;
    CHECK(histogramScale(histogram, HistogramChannels::Rgb) == 5);
}

TEST_CASE("the scale of RGB is the largest of the channels' scales",
          "[app][histogram][presentation]") {
    OutputHistogram histogram;
    for (std::size_t bin = 1; bin < 255; ++bin) {
        histogram.red[bin] = 3;
        histogram.green[bin] = 40;
        histogram.blue[bin] = 7;
        histogram.luma[bin] = 11;
    }
    CHECK(histogramScale(histogram, HistogramChannels::Rgb) == 40);
    CHECK(histogramScale(histogram, HistogramChannels::Luma) == 11);
}

TEST_CASE("an empty interior gives a zero scale", "[app][histogram][presentation]") {
    OutputHistogram histogram;
    histogram.red[0] = 50;
    histogram.red[255] = 50;
    histogram.luma[0] = 50;
    histogram.luma[255] = 50;
    CHECK(histogramScale(histogram, HistogramChannels::Rgb) == 0);
    CHECK(histogramScale(histogram, HistogramChannels::Luma) == 0);
}

TEST_CASE("clipped channels name the channels whose end bin is non-zero",
          "[app][histogram][presentation]") {
    OutputHistogram histogram;
    CHECK(clippedChannels(histogram) == ClippedChannels{});
    CHECK_FALSE(clippedChannels(histogram).shadows.any());

    histogram.red[0] = 1;
    histogram.blue[0] = 4;
    histogram.green[255] = 2;
    const ClippedChannels clipped = clippedChannels(histogram);
    CHECK(clipped.shadows == ChannelSet{true, false, true});
    CHECK(clipped.highlights == ChannelSet{false, true, false});

    // Interior bins do not clip.
    OutputHistogram interior;
    interior.red[1] = interior.green[254] = 9;
    CHECK(clippedChannels(interior) == ClippedChannels{});
}

TEST_CASE("clipped shares are percentages of the counted pixels",
          "[app][histogram][presentation]") {
    OutputHistogram histogram;
    histogram.pixels = 1000;
    histogram.highlightsClipped = 12;
    histogram.red[255] = 11;
    histogram.green[255] = 4;
    histogram.blue[255] = 2;
    const ClippedShare share = clippedShare(histogram, ClipKind::Highlights);
    CHECK(share.any == 1.2);
    CHECK(share.red == 1.1);
    CHECK(share.green == 0.4);
    CHECK(share.blue == 0.2);
    CHECK(clippedShare(histogram, ClipKind::Shadows).any == 0.0);
    CHECK(clippedShare(OutputHistogram{}, ClipKind::Shadows).any == 0.0);
}

TEST_CASE("clip colours add the channels that clip", "[app][histogram][presentation]") {
    const QColor red = clipColour({true, false, false});
    const QColor green = clipColour({false, true, false});
    const QColor blue = clipColour({false, false, true});
    CHECK(clipColour({true, true, false}) == QColor(std::max(red.red(), green.red()),
                                                    std::max(red.green(), green.green()),
                                                    std::max(red.blue(), green.blue())));
    CHECK(clipColour({false, true, true}).red() < clipColour({false, true, true}).green());
    CHECK(clipColour({true, false, true}).green() < clipColour({true, false, true}).red());
    CHECK(clipColour({true, true, true}) == QColor(Qt::white));
    CHECK(red != green);
    CHECK(green != blue);
}

TEST_CASE("the clipped text gives the share of pixels and of each channel",
          "[app][histogram][presentation]") {
    OutputHistogram histogram;
    histogram.pixels = 1000;
    histogram.highlightsClipped = 12;
    histogram.red[255] = 11;
    histogram.green[255] = 4;
    histogram.blue[255] = 2;
    CHECK(clippedText(histogram, ClipKind::Highlights) ==
          "Highlights clipped: 1.2 % of pixels (R 1.1 %, G 0.4 %, B 0.2 %)");
    CHECK(clippedText(histogram, ClipKind::Shadows) == "Shadows clipped: none");

    // A share that rounds to nothing is still shown as something.
    histogram.pixels = 1'000'000;
    histogram.shadowsClipped = 3;
    histogram.red[0] = 3;
    CHECK(clippedText(histogram, ClipKind::Shadows).contains("<0.1 % of pixels"));
}

TEST_CASE("readout shows one decimal and dashes off the picture",
          "[app][histogram][presentation]") {
    CHECK(readoutText(qRgb(115, 102, 97)) == "R 45.1 %   G 40.0 %   B 38.0 %");
    CHECK(readoutText(qRgb(0, 255, 128)) == "R 0.0 %   G 100.0 %   B 50.2 %");
    const QString dashes = readoutText(std::nullopt);
    CHECK(dashes == QString::fromUtf8("R – %   G – %   B – %"));
    CHECK_FALSE(dashes.contains('0'));
}
