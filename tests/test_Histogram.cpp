// Regression guard for the readback→bin contract (docs/adr/0004, issue #51).
// The async readback path (ADR 0033) changes *when* samples arrive, not how
// they are binned — these pin the binning so a plumbing change can't silently
// corrupt the histogram. HistogramBins is the pure value type the Histogram
// widget delegates to; testing it keeps the GPU out of the loop.
#include "Histogram.h"
#include <catch2/catch_test_macros.hpp>
#include <QImage>

namespace {

// A 1×1 sRGB-linear sample (the histoRaw RGBA32F readback format).
QImage linearPixel(float rv, float gv, float bv) {
    QImage img(1, 1, QImage::Format_RGBX32FPx4);
    auto* px = reinterpret_cast<float*>(img.bits());
    px[0] = rv;
    px[1] = gv;
    px[2] = bv;
    px[3] = 1.0f;
    return img;
}

} // namespace

TEST_CASE("HistogramBins maps linear endpoints to encoded bins", "[histogram]") {
    HistogramBins black;
    black.bin(linearPixel(0.0f, 0.0f, 0.0f));
    CHECK(black.r[0] == 1);
    CHECK(black.g[0] == 1);
    CHECK(black.b[0] == 1);

    HistogramBins white;
    white.bin(linearPixel(1.0f, 1.0f, 1.0f));
    CHECK(white.r[255] == 1);
    CHECK(white.g[255] == 1);
    CHECK(white.b[255] == 1);
}

TEST_CASE("HistogramBins tallies out-of-range linear values as over/underflow", "[histogram]") {
    HistogramBins bins;
    // r overflows (>1), g underflows (<0), b in range — only b lands in a bin.
    bins.bin(linearPixel(2.0f, -0.5f, 0.5f));

    CHECK(bins.rOver == 1);
    CHECK(bins.gUnder == 1);
    CHECK(bins.bOver == 0);
    CHECK(bins.bUnder == 0);

    // Nothing leaked into the in-range bins for the clamped channels.
    int rBinned = 0, gBinned = 0;
    for (int i = 0; i < HistogramBins::kBins; ++i) {
        rBinned += bins.r[i];
        gBinned += bins.g[i];
    }
    CHECK(rBinned == 0);
    CHECK(gBinned == 0);
}

TEST_CASE("HistogramBins bins an 8-bit sample by channel value", "[histogram]") {
    // The curve-input sample arrives as already-encoded RGBA8 — binned directly,
    // no sRGB gamma applied.
    QImage img(1, 1, QImage::Format_RGBA8888);
    img.setPixelColor(0, 0, QColor(10, 20, 30));

    HistogramBins bins;
    bins.bin(img);

    CHECK(bins.r[10] == 1);
    CHECK(bins.g[20] == 1);
    CHECK(bins.b[30] == 1);
}

TEST_CASE("HistogramBins peak is the largest bin or overflow tally", "[histogram]") {
    // Three identical black pixels stack in bin 0; peak follows the tallest count.
    QImage img(3, 1, QImage::Format_RGBX32FPx4);
    auto* px = reinterpret_cast<float*>(img.bits());
    for (int i = 0; i < 3; ++i) {
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = 0.0f;
        px[i * 4 + 3] = 1.0f;
    }

    HistogramBins bins;
    bins.bin(img);
    CHECK(bins.r[0] == 3);
    CHECK(bins.peak == 3);
}

TEST_CASE("HistogramBins clears previous counts when re-binned", "[histogram]") {
    HistogramBins bins;
    bins.bin(linearPixel(1.0f, 1.0f, 1.0f)); // fills bin 255
    bins.bin(linearPixel(0.0f, 0.0f, 0.0f)); // should leave only bin 0

    CHECK(bins.r[255] == 0);
    CHECK(bins.r[0] == 1);
}
