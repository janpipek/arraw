#include "ClippingMask.h"
#include "ui/ThemeColors.h"

#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <utility>

using namespace arraw::app;

/// The overlay that paints clipped pixels on the picture (ADR 055, section 5).

namespace {

/// Builds a one-row RGBA8888 picture from the given colours.
QImage row(std::initializer_list<QRgb> colours) {
    QImage image(static_cast<int>(colours.size()), 1, QImage::Format_RGBA8888);
    int x = 0;
    for (const QRgb colour : colours) {
        image.setPixel(x++, 0, colour);
    }
    return image;
}

const QRgb highlightPaint = theme::clippingHighlights.rgba();
const QRgb shadowPaint = theme::clippingShadows.rgba();

} // namespace

TEST_CASE("any channel at 255 is painted red", "[app][clipping]") {
    const QImage mask =
        clippingMask(row({qRgb(255, 10, 10), qRgb(10, 255, 10), qRgb(10, 10, 255)}), true, true);
    REQUIRE(mask.format() == QImage::Format_ARGB32_Premultiplied);
    for (int x = 0; x < 3; ++x) {
        CHECK(mask.pixel(x, 0) == highlightPaint);
    }
    CHECK(qRed(highlightPaint) > qBlue(highlightPaint));
}

TEST_CASE("any channel at 0 is painted blue", "[app][clipping]") {
    const QImage mask =
        clippingMask(row({qRgb(0, 100, 100), qRgb(100, 0, 100), qRgb(100, 100, 0)}), true, true);
    for (int x = 0; x < 3; ++x) {
        CHECK(mask.pixel(x, 0) == shadowPaint);
    }
    CHECK(qBlue(shadowPaint) > qRed(shadowPaint));
}

TEST_CASE("a pixel at both is painted as highlights", "[app][clipping]") {
    const QImage mask = clippingMask(row({qRgb(0, 255, 100)}), true, true);
    CHECK(mask.pixel(0, 0) == highlightPaint);
}

TEST_CASE("the other pixels are transparent", "[app][clipping]") {
    const QImage mask = clippingMask(
        row({qRgb(1, 1, 1), qRgb(254, 254, 254), qRgb(128, 64, 200), qRgba(255, 255, 255, 0)}),
        true, true);
    REQUIRE(mask.size() == QSize(4, 1));
    for (int x = 0; x < 4; ++x) {
        CHECK(qAlpha(mask.pixel(x, 0)) == 0);
    }
    CHECK(clippingMask(QImage(), true, true).isNull());
}

TEST_CASE("turned-off kinds are not painted", "[app][clipping]") {
    const QImage picture = row({qRgb(255, 100, 100), qRgb(0, 100, 100), qRgb(0, 255, 100)});
    const QImage onlyHighlights = clippingMask(picture, false, true);
    CHECK(onlyHighlights.pixel(0, 0) == highlightPaint);
    CHECK(qAlpha(onlyHighlights.pixel(1, 0)) == 0);
    CHECK(onlyHighlights.pixel(2, 0) == highlightPaint);

    const QImage onlyShadows = clippingMask(picture, true, false);
    CHECK(qAlpha(onlyShadows.pixel(0, 0)) == 0);
    CHECK(onlyShadows.pixel(1, 0) == shadowPaint);
    // Clipped at both ends, with only shadows on: it is still shadow-clipped.
    CHECK(onlyShadows.pixel(2, 0) == shadowPaint);

    const QImage neither = clippingMask(picture, false, false);
    for (int x = 0; x < 3; ++x) {
        CHECK(qAlpha(neither.pixel(x, 0)) == 0);
    }
}

TEST_CASE("a picture in another format is read as its colours", "[app][clipping]") {
    QImage picture(2, 1, QImage::Format_RGB32);
    picture.setPixel(0, 0, qRgb(255, 0, 0));
    picture.setPixel(1, 0, qRgb(9, 9, 9));
    const QImage mask = clippingMask(picture, true, true);
    CHECK(mask.pixel(0, 0) == highlightPaint);
    CHECK(qAlpha(mask.pixel(1, 0)) == 0);
}

TEST_CASE("clipping mask cost at 3840x2160", "[.bench][app][clipping]") {
    // Noise clips about 1 % of its samples at each end; a flat picture clips none or all.
    QImage noise(3840, 2160, QImage::Format_RGBA8888);
    std::uint32_t state = 12345;
    for (int y = 0; y < noise.height(); ++y) {
        auto* line = noise.scanLine(y);
        for (int x = 0; x < noise.width() * 4; ++x) {
            state = state * 1664525U + 1013904223U;
            line[x] = static_cast<uchar>(state >> 24);
        }
    }
    QImage flat(3840, 2160, QImage::Format_RGBA8888);
    flat.fill(QColor(100, 100, 100));
    for (const auto& [name, picture] : {std::pair<const char*, const QImage&>{"noise", noise},
                                        std::pair<const char*, const QImage&>{"flat", flat}}) {
        // The best of five: a shared machine only ever adds time.
        double best = 1e9;
        QImage mask;
        for (int run = 0; run < 5; ++run) {
            const auto start = std::chrono::steady_clock::now();
            mask = clippingMask(picture, true, true);
            best = std::min(best, std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());
        }
        WARN(name << " 3840x2160: " << best << " ms");
        CHECK(mask.size() == picture.size());
    }
}
