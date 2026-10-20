#include "OutputHistogramCount.h"
#include "support/TempDir.h"
#include "support/TestImages.h"

#include <ImageExport.h>
#include <OutputHistogram.h>

#include <QImage>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

using namespace arraw;

namespace {

/// @brief Builds an opaque or transparent-aware RGBA8 buffer from pixels.
ImageBuffer rgba8(ImageSize size, const std::vector<std::array<std::uint8_t, 4>>& pixels,
                  NamedEncoding encoding = NamedEncoding::Srgb) {
    ImageBuffer buffer(size, PixelFormat::RgbaU8, encoding);
    auto samples = buffer.samples<std::uint8_t>();
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
            samples[i * 4 + c] = pixels[i][c];
        }
    }
    return buffer;
}

/// @brief Builds a one-row RgbaF32 buffer of opaque pixels with the given red.
ImageBuffer floatReds(const std::vector<float>& reds) {
    ImageBuffer buffer({static_cast<std::uint32_t>(reds.size()), 1}, PixelFormat::RgbaF32,
                       NamedEncoding::Srgb);
    auto samples = buffer.samples<float>();
    for (std::size_t i = 0; i < reds.size(); ++i) {
        samples[i * 4] = reds[i];
        samples[i * 4 + 3] = 1.0F;
    }
    return buffer;
}

std::uint64_t total(const OutputHistogram::Bins& bins) {
    return std::accumulate(bins.begin(), bins.end(), std::uint64_t{0});
}

} // namespace

TEST_CASE("each 8-bit code lands in its own bin", "[OutputHistogram]") {
    ImageBuffer buffer({256, 1}, PixelFormat::RgbU8, NamedEncoding::Srgb);
    auto samples = buffer.samples<std::uint8_t>();
    for (std::size_t code = 0; code < 256; ++code) {
        samples[code * 3] = static_cast<std::uint8_t>(code);
        samples[code * 3 + 1] = static_cast<std::uint8_t>(255 - code);
        samples[code * 3 + 2] = 7;
    }
    const OutputHistogram histogram = outputHistogram(buffer);
    CHECK(histogram.pixels == 256);
    CHECK(histogram.encoding == NamedEncoding::Srgb);
    for (std::size_t bin = 0; bin < 256; ++bin) {
        CHECK(histogram.red[bin] == 1);
        CHECK(histogram.green[bin] == 1);
        CHECK(histogram.blue[bin] == (bin == 7 ? 256 : 0));
    }
}

TEST_CASE("a grey's luma is its own code in every output encoding", "[OutputHistogram]") {
    const auto encoding =
        GENERATE(NamedEncoding::Srgb, NamedEncoding::DisplayP3, NamedEncoding::AdobeRgb);
    ImageBuffer buffer({256, 1}, PixelFormat::RgbU8, encoding);
    auto samples = buffer.samples<std::uint8_t>();
    for (std::size_t code = 0; code < 256; ++code) {
        samples[code * 3] = samples[code * 3 + 1] = samples[code * 3 + 2] =
            static_cast<std::uint8_t>(code);
    }
    const OutputHistogram histogram = outputHistogram(buffer);
    CHECK(histogram.encoding == encoding);
    for (std::size_t bin = 0; bin < 256; ++bin) {
        CHECK(histogram.luma[bin] == 1);
    }
}

TEST_CASE("pure red's luma is the encoded luminance, not a sum of encoded channels",
          "[OutputHistogram]") {
    const OutputHistogram histogram = outputHistogram(rgba8({1, 1}, {{255, 0, 0, 255}}));
    // Linear luminance 0.2127 encodes to 0.5 of the range: code 127 or 128, not 0.2126 * 255 = 54.
    std::size_t luma = 0;
    for (std::size_t bin = 0; bin < 256; ++bin) {
        if (histogram.luma[bin] != 0) {
            luma = bin;
        }
    }
    CHECK(total(histogram.luma) == 1);
    CHECK(luma == 127);
}

TEST_CASE("a pixel with any channel at 255 counts once as highlight-clipped", "[OutputHistogram]") {
    const OutputHistogram histogram = outputHistogram(
        rgba8({3, 1}, {{255, 255, 255, 255}, {10, 255, 20, 255}, {10, 254, 20, 255}}));
    CHECK(histogram.highlightsClipped == 2);
    CHECK(histogram.shadowsClipped == 0);
}

TEST_CASE("a pixel with any channel at 0 counts once as shadow-clipped", "[OutputHistogram]") {
    const OutputHistogram histogram =
        outputHistogram(rgba8({3, 1}, {{0, 0, 0, 255}, {10, 0, 20, 255}, {10, 1, 20, 255}}));
    CHECK(histogram.shadowsClipped == 2);
    CHECK(histogram.highlightsClipped == 0);
}

TEST_CASE("a pixel at 0 and 255 counts as both", "[OutputHistogram]") {
    const OutputHistogram histogram = outputHistogram(rgba8({1, 1}, {{0, 255, 128, 255}}));
    CHECK(histogram.shadowsClipped == 1);
    CHECK(histogram.highlightsClipped == 1);
    CHECK(histogram.pixels == 1);
}

TEST_CASE("float and 16-bit samples round to the nearest code", "[OutputHistogram]") {
    SECTION("float") {
        const OutputHistogram histogram = outputHistogram(floatReds(
            {254.4F / 255.0F, 254.6F / 255.0F, 0.4F / 255.0F, 0.6F / 255.0F, -1.0F, 2.0F}));
        CHECK(histogram.red[254] == 1);
        CHECK(histogram.red[255] == 2);
        CHECK(histogram.red[0] == 2);
        CHECK(histogram.red[1] == 1);
    }
    SECTION("16 bit") {
        ImageBuffer buffer({4, 1}, PixelFormat::RgbU16, NamedEncoding::Srgb);
        auto samples = buffer.samples<std::uint16_t>();
        // 254.4/255 and 254.6/255 of 65535, then 0.4/255 and 0.6/255.
        const std::array<std::uint16_t, 4> reds{65382, 65433, 103, 154};
        for (std::size_t i = 0; i < 4; ++i) {
            samples[i * 3] = reds[i];
        }
        const OutputHistogram histogram = outputHistogram(buffer);
        CHECK(histogram.red[254] == 1);
        CHECK(histogram.red[255] == 1);
        CHECK(histogram.red[0] == 1);
        CHECK(histogram.red[1] == 1);
    }
}

TEST_CASE("fully transparent pixels are not counted", "[OutputHistogram]") {
    const OutputHistogram histogram =
        outputHistogram(rgba8({3, 1}, {{0, 0, 0, 0}, {255, 255, 255, 0}, {9, 9, 9, 1}}));
    CHECK(histogram.pixels == 1);
    CHECK(total(histogram.red) == 1);
    CHECK(total(histogram.luma) == 1);
    CHECK(histogram.shadowsClipped == 0);
    CHECK(histogram.highlightsClipped == 0);
}

TEST_CASE("spans restrict the count to their pixels", "[OutputHistogram]") {
    ImageBuffer buffer({4, 3}, PixelFormat::RgbU8, NamedEncoding::Srgb);
    auto samples = buffer.samples<std::uint8_t>();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::uint8_t>(i / 3); // pixel index as the code
    }
    const std::vector<RowSpan> spans{{1, 3}, {0, 0}, {3, 4}};
    const OutputHistogram histogram =
        countOutputCodes(reinterpret_cast<const std::byte*>(buffer.bytes().data()), buffer.size(),
                         buffer.rowStride(), buffer.format(), NamedEncoding::Srgb, spans);
    CHECK(histogram.pixels == 3);
    CHECK(histogram.red[1] == 1);
    CHECK(histogram.red[2] == 1);
    CHECK(histogram.red[11] == 1);
    CHECK(histogram.red[5] == 0);

    CHECK_THROWS_AS(countOutputCodes(reinterpret_cast<const std::byte*>(buffer.bytes().data()),
                                     buffer.size(), buffer.rowStride(), buffer.format(),
                                     NamedEncoding::Srgb, std::span(spans).first(2)),
                    std::invalid_argument);
}

TEST_CASE("working, camera and perceptual encodings are refused by the counting overload",
          "[OutputHistogram]") {
    CHECK_THROWS_AS(
        outputHistogram(test::rainbow({4, 4}, PixelFormat::RgbaF32, NamedEncoding::LinearRec2020)),
        std::invalid_argument);
    CHECK_THROWS_AS(
        outputHistogram(test::rainbow({4, 4}, PixelFormat::RgbaF32, NamedEncoding::Rec2020Gamma22)),
        std::invalid_argument);
    ImageBuffer camera({2, 2}, PixelFormat::RgbaF32, CameraNative{});
    CHECK_THROWS_AS(outputHistogram(camera), std::invalid_argument);
    const auto bytes = std::vector<std::byte>(16);
    CHECK_THROWS_AS(countOutputCodes(bytes.data(), {2, 2}, 8, PixelFormat::RgbaU8,
                                     NamedEncoding::LinearRec2020),
                    std::invalid_argument);
}

TEST_CASE("the converting overload equals counting an unsharpened 8-bit export",
          "[OutputHistogram]") {
    const auto encoding =
        GENERATE(NamedEncoding::Srgb, NamedEncoding::DisplayP3, NamedEncoding::AdobeRgb);
    const ImageBuffer developed =
        test::rainbow({64, 48}, PixelFormat::RgbaF32, NamedEncoding::LinearRec2020);

    const test::TempDir directory;
    const auto path = directory.file("export.png");
    ExportOptions options;
    options.encoding = encoding;
    options.bitDepth = 8;
    options.sharpening = 0;
    exportImage(developed, path, options);

    QImage exported(QString::fromStdString(path.string()));
    REQUIRE_FALSE(exported.isNull());
    exported = exported.convertedTo(QImage::Format_RGBA8888);
    const OutputHistogram expected = countOutputCodes(
        reinterpret_cast<const std::byte*>(exported.constBits()),
        {static_cast<std::uint32_t>(exported.width()),
         static_cast<std::uint32_t>(exported.height())},
        static_cast<std::size_t>(exported.bytesPerLine()), PixelFormat::RgbaU8, encoding);

    CHECK(outputHistogram(developed, encoding) == expected);
    CHECK_THROWS_AS(outputHistogram(developed, NamedEncoding::LinearRec2020),
                    std::invalid_argument);
}

TEST_CASE("an abandoned count stops within a band of rows", "[OutputHistogram]") {
    ImageBuffer buffer({8, 256}, PixelFormat::RgbU8, NamedEncoding::Srgb);
    const auto* rows = reinterpret_cast<const std::byte*>(buffer.bytes().data());
    const std::atomic<bool> abandon{true};
    const OutputHistogram stopped =
        countOutputCodes(rows, buffer.size(), buffer.rowStride(), buffer.format(),
                         NamedEncoding::Srgb, {}, &abandon);
    CHECK(stopped.pixels < buffer.size().pixelCount());
    CHECK(stopped.pixels <= std::uint64_t{outputCountBandRows} * 8);

    const std::atomic<bool> keep{false};
    CHECK(countOutputCodes(rows, buffer.size(), buffer.rowStride(), buffer.format(),
                           NamedEncoding::Srgb, {}, &keep)
              .pixels == buffer.size().pixelCount());
}

TEST_CASE("count cost at 2560x1600 and 3840x2160", "[.bench][OutputHistogram]") {
    for (const ImageSize size : {ImageSize{2560, 1600}, ImageSize{3840, 2160}}) {
        const ImageBuffer picture = test::rainbow(size, PixelFormat::RgbaU8, NamedEncoding::Srgb);
        const auto start = std::chrono::steady_clock::now();
        const OutputHistogram histogram = outputHistogram(picture);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double milliseconds = std::chrono::duration<double, std::milli>(elapsed).count();
        WARN(size.width << "x" << size.height << ": " << milliseconds << " ms");
        CHECK(histogram.pixels == size.pixelCount());
    }
}
