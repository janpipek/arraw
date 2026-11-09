#include "ColorSpaces.h"
#include "EncodedForOutput.h"
#include "support/RowBandLimit.h"

#include <Diagnostics.h>
#include <ImageBuffer.h>

#include <QColorSpace>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace arraw;
using Catch::Approx;

namespace {

/// Straight-alpha pixel of floats.
using Rgba = std::array<float, 4>;

/// @brief Builds a float buffer from pixels, one row.
ImageBuffer floatImage(const std::vector<Rgba>& pixels, NamedEncoding encoding) {
    ImageBuffer image({static_cast<std::uint32_t>(pixels.size()), 1}, PixelFormat::RgbaF32,
                      encoding);
    std::memcpy(image.bytes().data(), pixels.data(), pixels.size() * sizeof(Rgba));
    return image;
}

/// @brief Reads one pixel of a 16-bit-per-channel result.
std::array<std::uint16_t, 4> codes16(const QImage& image, int x, int y = 0) {
    std::array<std::uint16_t, 4> p{};
    std::memcpy(p.data(), image.constScanLine(y) + 8 * x, sizeof p);
    return p;
}

/// @brief Reads one pixel of a float result.
Rgba floats(const QImage& image, int x, int y = 0) {
    Rgba p{};
    std::memcpy(p.data(), image.constScanLine(y) + 16 * x, sizeof p);
    return p;
}

/// @brief sRGB encoding, written here independently of the library's.
double refEncodeSrgb(double v) {
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

double refEncodeAdobe(double v) {
    return std::pow(v, 256.0 / 563.0);
}

/// @brief Linear Rec.2020 to linear target, by solving for the primaries in the test's own way.
///
/// Primaries and white as chromaticities; RGB-to-XYZ found by Cramer's rule on the white point,
/// not by the library's inverse.
using Mat = std::array<std::array<double, 3>, 3>;

double det(const Mat& m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

Mat rgbToXyz(std::array<std::array<double, 2>, 3> xy) {
    Mat cols{};
    for (int i = 0; i < 3; ++i) {
        cols[0][i] = xy[i][0] / xy[i][1];
        cols[1][i] = 1.0;
        cols[2][i] = (1.0 - xy[i][0] - xy[i][1]) / xy[i][1];
    }
    const std::array<double, 3> white{0.3127 / 0.3290, 1.0, (1.0 - 0.3127 - 0.3290) / 0.3290};
    const double d = det(cols);
    std::array<double, 3> scale{};
    for (int i = 0; i < 3; ++i) {
        Mat replaced = cols;
        for (int r = 0; r < 3; ++r) {
            replaced[r][i] = white[r];
        }
        scale[i] = det(replaced) / d;
    }
    Mat result{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            result[r][c] = cols[r][c] * scale[c];
        }
    }
    return result;
}

Mat inverse(const Mat& m) {
    const double d = det(m);
    Mat r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const int r0 = (j + 1) % 3, r1 = (j + 2) % 3, c0 = (i + 1) % 3, c1 = (i + 2) % 3;
            r[i][j] = (m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]) / d;
        }
    }
    return r;
}

Mat rec2020ToXyz() {
    return rgbToXyz({{{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}}});
}

/// @brief Linear Rec.2020 into a target's linear primaries.
std::array<double, 3> toPrimaries(const Mat& targetToXyz, std::array<double, 3> rgb) {
    const Mat to = inverse(targetToXyz);
    const Mat from = rec2020ToXyz();
    std::array<double, 3> xyz{};
    std::array<double, 3> out{};
    for (int r = 0; r < 3; ++r) {
        xyz[r] = from[r][0] * rgb[0] + from[r][1] * rgb[1] + from[r][2] * rgb[2];
    }
    for (int r = 0; r < 3; ++r) {
        out[r] = to[r][0] * xyz[0] + to[r][1] * xyz[1] + to[r][2] * xyz[2];
    }
    return out;
}

/// @brief Deterministic colours spread over the gamut of every target, the edges included.
std::vector<Rgba> colours(int count) {
    std::vector<Rgba> pixels;
    std::uint32_t state = 12345;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(state >> 8) / static_cast<float>(1U << 24);
    };
    for (int i = 0; i < count; ++i) {
        // Cubed, so that the dark end, where curves are steep, is sampled densely.
        const float a = next();
        const float b = next();
        const float c = next();
        pixels.push_back({a * a * a, b * b, c, next()});
    }
    return pixels;
}

} // namespace

TEST_CASE("A fine linear ramp keeps every 16-bit code the sRGB curve gives it",
          "[EncodedForOutput]") {
    // Qt's conversion collapsed a +1% ramp at 0.001 into one code and gave steps of 13 to 41
    // codes at 0.05 and 0.36 (ADR 057). The exact conversion rounds once.
    for (const double start : {0.001, 0.05, 0.36, 0.9}) {
        constexpr int samples = 20000;
        std::vector<Rgba> ramp;
        for (int i = 0; i < samples; ++i) {
            const auto v = static_cast<float>(start * (1.0 + 0.01 * i / (samples - 1)));
            ramp.push_back({v, v, v, 1.0F});
        }
        const QImage out = encodedForOutput(floatImage(ramp, NamedEncoding::LinearRec2020),
                                            NamedEncoding::Srgb, QImage::Format_RGBA64);
        std::vector<std::uint16_t> distinct;
        for (int i = 0; i < samples; ++i) {
            // Grey: the primaries conversion keeps white, so only the curve is left.
            const double expected = std::floor(65535.0 * refEncodeSrgb(ramp[i][0]) + 0.5);
            const auto p = codes16(out, i);
            REQUIRE(p[0] == expected);
            REQUIRE(p[1] == p[0]);
            REQUIRE(p[2] == p[0]);
            if (distinct.empty() || distinct.back() != p[0]) {
                distinct.push_back(p[0]);
            }
        }
        // The curve is monotonic, so runs of equal codes are the distinct codes. A 1% span at
        // v encodes to (span of the curve) codes, the exact formula's count; Qt gave 1, 3 and 15
        // here.
        const double first = std::floor(65535.0 * refEncodeSrgb(ramp.front()[0]) + 0.5);
        const double last = std::floor(65535.0 * refEncodeSrgb(ramp.back()[0]) + 0.5);
        REQUIRE(distinct.size() == static_cast<std::size_t>(last - first + 1));
        if (start >= 0.05) {
            REQUIRE(distinct.size() > 50);
        }
    }
}

TEST_CASE("Linear Rec.2020 converts to each output encoding exactly, primaries included",
          "[EncodedForOutput]") {
    const auto pixels = colours(4000);
    const auto image = floatImage(pixels, NamedEncoding::LinearRec2020);

    struct Target {
        NamedEncoding encoding;
        Mat toXyz;
        double (*encode)(double);
    };
    const std::array<Target, 3> targets{
        {{NamedEncoding::Srgb, rgbToXyz({{{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}}}),
          refEncodeSrgb},
         {NamedEncoding::DisplayP3, rgbToXyz({{{0.68, 0.32}, {0.265, 0.69}, {0.15, 0.06}}}),
          refEncodeSrgb},
         {NamedEncoding::AdobeRgb, rgbToXyz({{{0.64, 0.33}, {0.21, 0.71}, {0.15, 0.06}}}),
          refEncodeAdobe}}};
    for (const auto& target : targets) {
        const QImage out = encodedForOutput(image, target.encoding, QImage::Format_RGBA64);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const auto linear =
                toPrimaries(target.toXyz, {pixels[i][0], pixels[i][1], pixels[i][2]});
            const auto p = codes16(out, static_cast<int>(i));
            for (std::size_t c = 0; c < 3; ++c) {
                const double clamped = std::clamp(linear[c], 0.0, 1.0);
                const double expected = std::floor(65535.0 * target.encode(clamped) + 0.5);
                REQUIRE(static_cast<double>(p[c]) == expected);
            }
        }
    }
}

TEST_CASE("sRGB red in Display P3 is the published colour", "[EncodedForOutput]") {
    ImageBuffer srgb({1, 1}, PixelFormat::RgbaU8, NamedEncoding::Srgb);
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    std::memcpy(srgb.bytes().data(), red.data(), 4);
    const QImage out = encodedForOutput(srgb, NamedEncoding::DisplayP3, QImage::Format_RGBA64);
    const auto p = codes16(out, 0);
    REQUIRE(std::abs(p[0] - 0.91749 * 65535) < 1.0);
    REQUIRE(std::abs(p[1] - 0.20029 * 65535) < 1.0);
    REQUIRE(std::abs(p[2] - 0.13856 * 65535) < 1.0);
}

TEST_CASE("The primaries derived from chromaticities agree with the working-space matrices",
          "[EncodedForOutput]") {
    const auto derived = colorspaces::between(NamedEncoding::Srgb, NamedEncoding::LinearRec2020);
    for (std::size_t i = 0; i < 9; ++i) {
        REQUIRE(derived[i] == Approx(colorspaces::srgbToWorking.values[i]).margin(2e-6));
    }
    const auto back = colorspaces::between(NamedEncoding::LinearRec2020, NamedEncoding::Srgb);
    for (std::size_t i = 0; i < 9; ++i) {
        REQUIRE(back[i] == Approx(colorspaces::workingToSrgb.values[i]).margin(2e-5));
    }
    // White stays white between any two spaces.
    for (const auto from :
         {NamedEncoding::Srgb, NamedEncoding::DisplayP3, NamedEncoding::AdobeRgb}) {
        const auto m = colorspaces::between(from, NamedEncoding::LinearRec2020);
        for (std::size_t row = 0; row < 3; ++row) {
            REQUIRE(m[row * 3] + m[row * 3 + 1] + m[row * 3 + 2] == Approx(1.0).margin(1e-12));
        }
    }
}

TEST_CASE("Integer outputs clamp each channel, floats pass out-of-range values through",
          "[EncodedForOutput]") {
    // Linear Rec.2020 (1.5, 0.5, 0.2) and (-0.1, 0.5, 0.2) in sRGB, as Qt gave them.
    const auto image = floatImage({{1.5F, 0.5F, 0.2F, 1.0F}, {-0.1F, 0.5F, 0.2F, 1.0F}},
                                  NamedEncoding::LinearRec2020);

    const QImage f = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA32FPx4);
    REQUIRE(floats(f, 0)[0] == Approx(1.40540F).margin(2e-5));
    REQUIRE(floats(f, 0)[1] == Approx(0.64837F).margin(2e-5));
    REQUIRE(floats(f, 0)[2] == Approx(0.41852F).margin(2e-5));
    REQUIRE(floats(f, 1)[0] == Approx(-0.71827F).margin(2e-5));
    REQUIRE(floats(f, 1)[1] == Approx(0.78410F).margin(2e-5));
    REQUIRE(floats(f, 1)[2] == Approx(0.45565F).margin(2e-5));

    const QImage i16 = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA64);
    REQUIRE(codes16(i16, 0)[0] == 65535);
    REQUIRE(codes16(i16, 1)[0] == 0);
    const QImage i8 = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA8888);
    REQUIRE(i8.constScanLine(0)[0] == 255);
    REQUIRE(i8.constScanLine(0)[1] == 165); // sRGB 0.64837 * 255 = 165.3
    REQUIRE(i8.constScanLine(0)[2] == 107); // 0.41852 * 255 = 106.7
    REQUIRE(i8.constScanLine(0)[4] == 0);
}

TEST_CASE("Alpha stays straight and is only rounded to the depth", "[EncodedForOutput]") {
    const auto image = floatImage({{0.2F, 0.2F, 0.2F, 0.5F}}, NamedEncoding::LinearRec2020);
    const QImage i8 = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA8888);
    REQUIRE(i8.constScanLine(0)[3] == 128);
    const QImage i16 = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA64);
    REQUIRE(codes16(i16, 0)[3] == 32768);
    const QImage f = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA32FPx4);
    REQUIRE(floats(f, 0)[3] == 0.5F);
    // Straight: the colour is what it is at full alpha.
    const auto opaque = floatImage({{0.2F, 0.2F, 0.2F, 1.0F}}, NamedEncoding::LinearRec2020);
    const QImage o8 = encodedForOutput(opaque, NamedEncoding::Srgb, QImage::Format_RGBA8888);
    REQUIRE(i8.constScanLine(0)[0] == o8.constScanLine(0)[0]);
}

TEST_CASE("Layouts without alpha drop it, and sources without alpha are opaque",
          "[EncodedForOutput]") {
    const auto image = floatImage({{0.2F, 0.2F, 0.2F, 0.5F}}, NamedEncoding::LinearRec2020);
    const QImage rgb = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGB888);
    REQUIRE(rgb.format() == QImage::Format_RGB888);
    REQUIRE(rgb.constScanLine(0)[0] == 124); // sRGB of 0.2 is 0.4845 -> 123.5
    const QImage x64 = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBX64);
    REQUIRE(codes16(x64, 0)[3] == 65535);

    ImageBuffer plain({1, 1}, PixelFormat::RgbU8, NamedEncoding::Srgb);
    const std::array<std::uint8_t, 3> grey{10, 20, 30};
    std::memcpy(plain.bytes().data(), grey.data(), 3);
    const QImage out = encodedForOutput(plain, NamedEncoding::Srgb, QImage::Format_RGBA8888);
    REQUIRE(out.constScanLine(0)[0] == 10);
    REQUIRE(out.constScanLine(0)[1] == 20);
    REQUIRE(out.constScanLine(0)[2] == 30);
    REQUIRE(out.constScanLine(0)[3] == 255);
}

TEST_CASE("8-bit output rounds to the nearest code", "[EncodedForOutput]") {
    std::vector<Rgba> ramp;
    for (int i = 0; i < 1000; ++i) {
        const auto v = static_cast<float>(i) / 999.0F;
        ramp.push_back({v, v, v, 1.0F});
    }
    const QImage out = encodedForOutput(floatImage(ramp, NamedEncoding::LinearRec2020),
                                        NamedEncoding::Srgb, QImage::Format_RGBA8888);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(out.constScanLine(0)[4 * i] ==
                static_cast<int>(std::floor(255.0 * refEncodeSrgb(ramp[i][0]) + 0.5)));
    }
}

TEST_CASE("Every source encoding decodes to linear", "[EncodedForOutput]") {
    const auto linearOf = [](NamedEncoding encoding, float value) {
        ImageBuffer image({1, 1}, PixelFormat::RgbaF32, encoding);
        const Rgba pixel{value, value, value, 1.0F};
        std::memcpy(image.bytes().data(), pixel.data(), sizeof pixel);
        return floats(
            encodedForOutput(image, NamedEncoding::LinearRec2020, QImage::Format_RGBA32FPx4), 0)[1];
    };
    REQUIRE(linearOf(NamedEncoding::LinearRec2020, 0.3F) == Approx(0.3F));
    // Grey is grey in every space: only the curve is left.
    REQUIRE(linearOf(NamedEncoding::Srgb, 0.5F) == Approx(0.2140411F).epsilon(1e-6));
    REQUIRE(linearOf(NamedEncoding::Srgb, 0.02F) == Approx(0.02F / 12.92F).epsilon(1e-6));
    REQUIRE(linearOf(NamedEncoding::DisplayP3, 0.5F) == Approx(0.2140411F).epsilon(1e-6));
    REQUIRE(linearOf(NamedEncoding::AdobeRgb, 0.5F) ==
            Approx(std::pow(0.5, 563.0 / 256.0)).epsilon(1e-6));
    REQUIRE(linearOf(NamedEncoding::Rec2020Gamma22, 0.5F) ==
            Approx(std::pow(0.5, 2.2)).epsilon(1e-6));
    // Odd below zero, so an out-of-gamut sample survives.
    REQUIRE(linearOf(NamedEncoding::Rec2020Gamma22, -0.5F) ==
            Approx(-std::pow(0.5, 2.2)).epsilon(1e-6));
}

TEST_CASE("Integer sources decode from their own range", "[EncodedForOutput]") {
    ImageBuffer image({1, 1}, PixelFormat::RgbaU16, NamedEncoding::LinearRec2020);
    const std::array<std::uint16_t, 4> pixel{65535, 32768, 0, 65535};
    std::memcpy(image.bytes().data(), pixel.data(), sizeof pixel);
    const QImage out =
        encodedForOutput(image, NamedEncoding::LinearRec2020, QImage::Format_RGBA32FPx4);
    REQUIRE(floats(out, 0)[0] == 1.0F);
    REQUIRE(floats(out, 0)[1] == Approx(32768.0F / 65535.0F));
    REQUIRE(floats(out, 0)[2] == 0.0F);
}

TEST_CASE("The result carries the target's colour space", "[EncodedForOutput]") {
    const auto image = floatImage({{0.2F, 0.2F, 0.2F, 1.0F}}, NamedEncoding::LinearRec2020);
    REQUIRE(encodedForOutput(image, NamedEncoding::DisplayP3, QImage::Format_RGBA64).colorSpace() ==
            QColorSpace(QColorSpace::DisplayP3));
    REQUIRE(encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGB888).colorSpace() ==
            QColorSpace(QColorSpace::SRgb));
}

TEST_CASE("Unsupported inputs are refused", "[EncodedForOutput]") {
    const auto image = floatImage({{0.2F, 0.2F, 0.2F, 1.0F}}, NamedEncoding::LinearRec2020);
    REQUIRE_THROWS_AS(encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_ARGB32),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_Invalid),
                      std::invalid_argument);
    const ImageBuffer camera({1, 1}, PixelFormat::RgbaU16, CameraNative{});
    REQUIRE_THROWS_AS(encodedForOutput(camera, NamedEncoding::Srgb, QImage::Format_RGBA64),
                      std::invalid_argument);
    const ImageBuffer threeChannel({1, 1}, PixelFormat::RgbU16, NamedEncoding::Srgb);
    REQUIRE_THROWS_AS(encodedForOutput(threeChannel, NamedEncoding::Srgb, QImage::Format_RGBA64),
                      std::invalid_argument);
}

TEST_CASE("Splitting the rows over threads gives the single-threaded bits", "[EncodedForOutput]") {
    ImageBuffer image({512, 256}, PixelFormat::RgbaF32, NamedEncoding::LinearRec2020);
    const auto pixels = colours(512 * 256);
    std::memcpy(image.bytes().data(), pixels.data(), pixels.size() * sizeof(Rgba));
    QImage single;
    {
        const test::ScopedRowBandLimit one(1);
        single = encodedForOutput(image, NamedEncoding::DisplayP3, QImage::Format_RGBA64);
    }
    const QImage many = encodedForOutput(image, NamedEncoding::DisplayP3, QImage::Format_RGBA64);
    for (int y = 0; y < 256; ++y) {
        REQUIRE(std::memcmp(single.constScanLine(y), many.constScanLine(y), 512 * 8) == 0);
    }
}

TEST_CASE("Measure converting 24 MP float to 16-bit sRGB", "[.bench][encoded-bench]") {
    ImageBuffer image({6000, 4000}, PixelFormat::RgbaF32, NamedEncoding::LinearRec2020);
    const auto row = colours(6000);
    for (int y = 0; y < 4000; ++y) {
        std::memcpy(image.bytes().data() + static_cast<std::size_t>(y) * image.rowStride(),
                    row.data(), row.size() * sizeof(Rgba));
    }
    for (int run = 0; run < 3; ++run) {
        const auto start = std::chrono::steady_clock::now();
        const QImage out = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA64);
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
        std::printf("encodedForOutput 24 MP RgbaF32 -> RGBA64 sRGB: %.3f s\n", took.count());
        REQUIRE(!out.isNull());
    }
}
