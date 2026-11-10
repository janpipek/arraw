#include "ColorSpaces.h"
#include "EncodedForOutput.h"
#include "ProcessingPlan.h"
#include "TransferFunctions.h"
#include "support/RowBandLimit.h"

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
#include <thread>
#include <utility>
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

/// @brief Adobe RGB (1998) encoding, written here independently of the library's.
double refEncodeAdobe(double v) {
    return std::pow(v, 256.0 / 563.0);
}

/// @brief 3x3 matrix, row by row, for the test's own colour maths.
using Mat = std::array<std::array<double, 3>, 3>;

/// @brief Gives the determinant of a matrix.
double det(const Mat& m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

/// @brief Gives the RGB-to-XYZ matrix of primaries and the D65 white, by Cramer's rule.
///
/// Primaries and white as chromaticities, solved here and not by the library's inverse.
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

/// @brief Gives the inverse of a matrix, by cofactors.
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

/// @brief Gives the RGB-to-XYZ matrix of Rec.2020.
Mat rec2020ToXyz() {
    return rgbToXyz({{{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}}});
}

/// @brief Gives a linear colour of one set of primaries in another's.
/// @param sourceToXyz RGB-to-XYZ matrix of the colour's own primaries.
/// @param targetToXyz RGB-to-XYZ matrix of the primaries it is wanted in.
std::array<double, 3> toPrimaries(const Mat& sourceToXyz, const Mat& targetToXyz,
                                  std::array<double, 3> rgb) {
    const Mat to = inverse(targetToXyz);
    const Mat& from = sourceToXyz;
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
            const auto linear = toPrimaries(rec2020ToXyz(), target.toXyz,
                                            {pixels[i][0], pixels[i][1], pixels[i][2]});
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

TEST_CASE("A non-linear source changes primaries over many pixels", "[EncodedForOutput]") {
    // Adobe RGB codes to sRGB: the source curve, the primaries and the target curve all act.
    constexpr std::uint32_t count = 3000;
    ImageBuffer image({count, 1}, PixelFormat::RgbaU16, NamedEncoding::AdobeRgb);
    std::vector<std::uint16_t> codes(count * 4);
    std::uint32_t state = 777;
    for (auto& code : codes) {
        state = state * 1664525U + 1013904223U;
        code = static_cast<std::uint16_t>(state >> 16);
    }
    std::memcpy(image.bytes().data(), codes.data(), codes.size() * sizeof(std::uint16_t));
    const Mat adobe = rgbToXyz({{{0.64, 0.33}, {0.21, 0.71}, {0.15, 0.06}}});
    const Mat srgb = rgbToXyz({{{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}}});

    const QImage out = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA64);

    for (std::uint32_t i = 0; i < count; ++i) {
        std::array<double, 3> linear{};
        for (std::size_t c = 0; c < 3; ++c) {
            linear[c] = std::pow(codes[i * 4 + c] / 65535.0, 563.0 / 256.0);
        }
        const auto converted = toPrimaries(adobe, srgb, linear);
        for (std::size_t c = 0; c < 3; ++c) {
            const double expected =
                std::floor(65535.0 * refEncodeSrgb(std::clamp(converted[c], 0.0, 1.0)) + 0.5);
            REQUIRE(static_cast<double>(codes16(out, static_cast<int>(i))[c]) == expected);
        }
    }
}

TEST_CASE("The gamma and linear Rec.2020 encodings are integer targets", "[EncodedForOutput]") {
    const auto pixels = colours(500);
    const auto image = floatImage(pixels, NamedEncoding::LinearRec2020);
    const QImage gamma =
        encodedForOutput(image, NamedEncoding::Rec2020Gamma22, QImage::Format_RGBA64);
    const QImage linear =
        encodedForOutput(image, NamedEncoding::LinearRec2020, QImage::Format_RGBA64);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const double v = std::clamp(static_cast<double>(pixels[i][c]), 0.0, 1.0);
            REQUIRE(codes16(gamma, static_cast<int>(i))[c] ==
                    std::floor(65535.0 * std::pow(v, 1.0 / 2.2) + 0.5));
            REQUIRE(codes16(linear, static_cast<int>(i))[c] == std::floor(65535.0 * v + 0.5));
        }
    }
}

TEST_CASE("RGB888 rows of an odd width and a padded stride convert pixel by pixel",
          "[EncodedForOutput]") {
    // 5 pixels of 3 bytes is 15 per row, which QImage pads to 16; the source rows have their own
    // stride. A wrong stride on either side shifts every row after the first.
    constexpr std::uint32_t width = 5;
    constexpr std::uint32_t height = 3;
    SECTION("float source") {
        ImageBuffer image({width, height}, PixelFormat::RgbaF32, NamedEncoding::LinearRec2020);
        std::vector<Rgba> pixels;
        for (std::uint32_t i = 0; i < width * height; ++i) {
            const float v = static_cast<float>(i + 1) / 16.0F;
            pixels.push_back({v, v, v, 1.0F});
        }
        std::memcpy(image.bytes().data(), pixels.data(), pixels.size() * sizeof(Rgba));
        const QImage out = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGB888);
        REQUIRE(out.size() == QSize(width, height));
        for (int y = 0; y < static_cast<int>(height); ++y) {
            for (int x = 0; x < static_cast<int>(width); ++x) {
                const double v = pixels[static_cast<std::size_t>(y) * width + x][0];
                const auto expected = static_cast<int>(std::floor(255.0 * refEncodeSrgb(v) + 0.5));
                for (int c = 0; c < 3; ++c) {
                    REQUIRE(out.constScanLine(y)[3 * x + c] == expected);
                }
            }
        }
    }
    SECTION("8-bit RGB source, same encoding") {
        ImageBuffer image({width, height}, PixelFormat::RgbU8, NamedEncoding::Srgb);
        auto samples = image.samples<std::uint8_t>();
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<std::uint8_t>(i * 5 + 3);
        }
        const QImage out = encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGB888);
        for (int y = 0; y < static_cast<int>(height); ++y) {
            for (int x = 0; x < static_cast<int>(width); ++x) {
                for (int c = 0; c < 3; ++c) {
                    REQUIRE(out.constScanLine(y)[3 * x + c] ==
                            samples[(static_cast<std::size_t>(y) * width + x) * 3 + c]);
                }
            }
        }
    }
}

TEST_CASE("Quantising a float image in its output encoding rounds it once", "[EncodedForOutput]") {
    constexpr int count = 400;
    std::vector<Rgba> pixels;
    for (int i = 0; i < count; ++i) {
        // From below zero to above one, so that the clamp is exercised on both sides.
        const float v = -0.1F + 1.2F * static_cast<float>(i) / (count - 1);
        pixels.push_back({v, 1.0F - v, v * 0.5F, std::clamp(v, 0.0F, 1.0F)});
    }
    pixels.push_back({std::nanf(""), 0.5F, 0.5F, 1.0F});
    QImage floatsImage(static_cast<int>(pixels.size()), 1, QImage::Format_RGBA32FPx4);
    floatsImage.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    std::memcpy(floatsImage.bits(), pixels.data(), pixels.size() * sizeof(Rgba));

    const QImage i16 = quantisedForOutput(floatsImage, QImage::Format_RGBA64);
    const QImage x64 = quantisedForOutput(floatsImage, QImage::Format_RGBX64);
    const QImage i8 = quantisedForOutput(floatsImage, QImage::Format_RGBA8888);
    const QImage rgb = quantisedForOutput(floatsImage, QImage::Format_RGB888);
    const QImage same = quantisedForOutput(floatsImage, QImage::Format_RGBA32FPx4);
    REQUIRE(i16.colorSpace() == QColorSpace(QColorSpace::DisplayP3));
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const int x = static_cast<int>(i);
        for (std::size_t c = 0; c < 4; ++c) {
            // Not-a-number is zero in an integer layout.
            const double v =
                std::isnan(pixels[i][c]) ? 0.0 : std::clamp<double>(pixels[i][c], 0.0, 1.0);
            REQUIRE(codes16(i16, x)[c] == std::floor(65535.0 * v + 0.5));
            REQUIRE(i8.constScanLine(0)[4 * x + static_cast<int>(c)] ==
                    std::floor(255.0 * v + 0.5));
            if (c < 3) {
                REQUIRE(codes16(x64, x)[c] == codes16(i16, x)[c]);
                REQUIRE(rgb.constScanLine(0)[3 * x + static_cast<int>(c)] ==
                        i8.constScanLine(0)[4 * x + static_cast<int>(c)]);
            }
        }
        REQUIRE(codes16(x64, x)[3] == 65535);
        if (!std::isnan(pixels[i][0])) {
            REQUIRE(floats(same, x)[0] == pixels[i][0]);
        }
    }
    QImage notFloat(1, 1, QImage::Format_RGBA64);
    REQUIRE_THROWS_AS(quantisedForOutput(notFloat, QImage::Format_RGBA8888), std::invalid_argument);
    REQUIRE_THROWS_AS(quantisedForOutput(floatsImage, QImage::Format_ARGB32),
                      std::invalid_argument);
}

TEST_CASE("The float perceptual curve is the double one", "[EncodedForOutput]") {
    // Two precisions of the one curve: the exponent comes from transfer::perceptualGamma.
    for (int i = 0; i <= 1000; ++i) {
        const float v = static_cast<float>(i) / 1000.0F;
        REQUIRE(toPerceptualSigned(v) == Approx(transfer::encodeGamma22(v)).epsilon(1e-6));
        REQUIRE(fromPerceptualSigned(v) == Approx(transfer::decodeGamma22(v)).epsilon(1e-6));
        REQUIRE(toPerceptualSigned(-v) == Approx(transfer::encodeGamma22(-v)).epsilon(1e-6));
    }
}

TEST_CASE("The luma weights of the output histogram follow the Rec.709 primaries",
          "[EncodedForOutput]") {
    // The published Rec.709 luma weights, which the derivation from the chromaticities meets to
    // within the rounding of the standard's own matrix.
    const auto y = colorspaces::rgbToXyz(colorspaces::primariesOf(NamedEncoding::Srgb));
    REQUIRE(y[3] == Approx(0.2126).margin(1e-4));
    REQUIRE(y[4] == Approx(0.7152).margin(1e-4));
    REQUIRE(y[5] == Approx(0.0722).margin(1e-4));
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

TEST_CASE("Measure the display conversion at window sizes, against Qt's",
          "[.bench][display-bench]") {
    // What a preview frame pays: working-encoding floats to 8-bit sRGB (ADR 057).
    const auto time = [](auto&& convert) {
        double best = 1e9;
        for (int run = 0; run < 5; ++run) {
            const auto start = std::chrono::steady_clock::now();
            const QImage out = convert();
            const std::chrono::duration<double, std::milli> took =
                std::chrono::steady_clock::now() - start;
            REQUIRE(!out.isNull());
            best = std::min(best, took.count());
        }
        return best;
    };
    for (const auto [width, height] : {std::pair{1920U, 1080U}, std::pair{2560U, 1440U},
                                       std::pair{3840U, 2160U}, std::pair{5120U, 2880U}}) {
        ImageBuffer image({width, height}, PixelFormat::RgbaF32, NamedEncoding::LinearRec2020);
        const auto row = colours(static_cast<int>(width));
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(image.bytes().data() + static_cast<std::size_t>(y) * image.rowStride(),
                        row.data(), row.size() * sizeof(Rgba));
        }
        QImage view(reinterpret_cast<const uchar*>(image.bytes().data()), static_cast<int>(width),
                    static_cast<int>(height), static_cast<qsizetype>(image.rowStride()),
                    QImage::Format_RGBA32FPx4);
        view.setColorSpace(
            QColorSpace(QColorSpace::Primaries::Bt2020, QColorSpace::TransferFunction::Linear));
        const double qt = time(
            [&] { return view.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_RGBA8888); });
        const double ours = time(
            [&] { return encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA8888); });
        std::printf("%ux%u RgbaF32 -> RGBA8888 sRGB: Qt %.1f ms, ours %.1f ms\n", width, height, qt,
                    ours);
    }
}

TEST_CASE("Measure concurrent display conversions", "[.bench][display-bench]") {
    constexpr std::uint32_t width = 3840;
    constexpr std::uint32_t height = 2160;
    ImageBuffer image({width, height}, PixelFormat::RgbaF32, NamedEncoding::LinearRec2020);
    const auto row = colours(width);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(image.bytes().data() + static_cast<std::size_t>(y) * image.rowStride(),
                    row.data(), row.size() * sizeof(Rgba));
    }
    for (const int callers : {1, 2, 4}) {
        const auto start = std::chrono::steady_clock::now();
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < callers; ++i) {
                threads.emplace_back([&] {
                    (void)encodedForOutput(image, NamedEncoding::Srgb, QImage::Format_RGBA8888);
                });
            }
        }
        const std::chrono::duration<double, std::milli> took =
            std::chrono::steady_clock::now() - start;
        std::printf("%d concurrent 3840x2160 conversions: %.1f ms wall\n", callers, took.count());
    }
}
