#include "core/ImageBuffer.h"
#include "pipeline/ColorManagement.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <QImage>

using Catch::Matchers::WithinAbs;

namespace {

// Wrap a 3-channel ImageBuffer as the Format_RGBX32FPx4 QImage that
// toOutputImage expects (the same packing renderToImage produces).
QImage toLinearImage(const ImageBuffer& buf) {
    QImage img(buf.width, buf.height, QImage::Format_RGBX32FPx4);
    for (int y = 0; y < buf.height; ++y) {
        float* dst = reinterpret_cast<float*>(img.scanLine(y));
        const float* src = buf.data.data() + size_t(y) * buf.width * 3;
        for (int x = 0; x < buf.width; ++x) {
            dst[x * 4 + 0] = src[x * 3 + 0];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 2];
            dst[x * 4 + 3] = 1.0f;
        }
    }
    return img;
}

} // namespace

TEST_CASE("sRGB white and black map to working-space 1 and 0", "[color]") {
    QImage img(2, 1, QImage::Format_RGB888);
    img.setPixelColor(0, 0, Qt::white);
    img.setPixelColor(1, 0, Qt::black);

    const auto buf = toWorkingSpaceBuffer(img);
    REQUIRE(buf.valid());
    for (int c = 0; c < 3; ++c) {
        CHECK_THAT(buf.data[c], WithinAbs(1.0, 2e-3));     // white
        CHECK_THAT(buf.data[3 + c], WithinAbs(0.0, 2e-3)); // black
    }
}

TEST_CASE("sRGB red lands on its known Rec.2020 coordinates", "[color]") {
    QImage img(1, 1, QImage::Format_RGB888);
    img.setPixelColor(0, 0, QColor(255, 0, 0));

    const auto buf = toWorkingSpaceBuffer(img);
    REQUIRE(buf.valid());
    // Columns of the standard linear sRGB→Rec.2020 matrix (both D65)
    CHECK_THAT(buf.data[0], WithinAbs(0.6274, 0.01));
    CHECK_THAT(buf.data[1], WithinAbs(0.0691, 0.01));
    CHECK_THAT(buf.data[2], WithinAbs(0.0164, 0.01));
}

TEST_CASE("linear mid-grey encodes to sRGB 188", "[color]") {
    // Grey is the same in any RGB space sharing the white point, so 0.5 linear
    // must come back as round(sRGB_encode(0.5) * 255) = 188.
    ImageBuffer grey;
    grey.width = 1;
    grey.height = 1;
    grey.data = {0.5f, 0.5f, 0.5f};

    const QImage out = toOutputImage(toLinearImage(grey), OutputProfile::SRgb, false);
    REQUIRE_FALSE(out.isNull());
    REQUIRE(out.format() == QImage::Format_RGB888);
    const QColor px = out.pixelColor(0, 0);
    CHECK(std::abs(px.red() - 188) <= 1);
    CHECK(std::abs(px.green() - 188) <= 1);
    CHECK(std::abs(px.blue() - 188) <= 1);
}

TEST_CASE("16-bit export keeps white at full scale", "[color]") {
    ImageBuffer white;
    white.width = 1;
    white.height = 1;
    white.data = {1.0f, 1.0f, 1.0f};

    const QImage out = toOutputImage(toLinearImage(white), OutputProfile::SRgb, true);
    REQUIRE(out.format() == QImage::Format_RGBA64);
    const QRgba64 px = out.pixelColor(0, 0).rgba64();
    CHECK(px.red() >= 65500);
    CHECK(px.green() >= 65500);
    CHECK(px.blue() >= 65500);
}

TEST_CASE("bounded export clips working-space headroom only at output encoding", "[color]") {
    ImageBuffer overWhite;
    overWhite.width = 1;
    overWhite.height = 1;
    overWhite.data = {1.2f, 1.2f, 1.2f};

    const QImage out = toOutputImage(toLinearImage(overWhite), OutputProfile::SRgb, false);
    REQUIRE(out.format() == QImage::Format_RGB888);
    const QColor px = out.pixelColor(0, 0);
    CHECK(px.red() == 255);
    CHECK(px.green() == 255);
    CHECK(px.blue() == 255);
}

TEST_CASE("sRGB through working space and back is identity within 8 bits", "[color]") {
    // toWorkingSpaceBuffer ∘ toOutputImage(SRgb) should reproduce the input —
    // this pins both directions of the lcms2 plumbing at once.
    QImage img(4, 1, QImage::Format_RGB888);
    img.setPixelColor(0, 0, QColor(200, 30, 90));
    img.setPixelColor(1, 0, QColor(15, 240, 128));
    img.setPixelColor(2, 0, QColor(70, 70, 200));
    img.setPixelColor(3, 0, QColor(128, 128, 128));

    const auto working = toWorkingSpaceBuffer(img);
    REQUIRE(working.valid());
    const QImage out = toOutputImage(toLinearImage(working), OutputProfile::SRgb, false);
    REQUIRE_FALSE(out.isNull());

    for (int x = 0; x < img.width(); ++x) {
        const QColor a = img.pixelColor(x, 0);
        const QColor b = out.pixelColor(x, 0);
        CHECK(std::abs(a.red() - b.red()) <= 1);
        CHECK(std::abs(a.green() - b.green()) <= 1);
        CHECK(std::abs(a.blue() - b.blue()) <= 1);
    }
}

TEST_CASE("display LUT without profiles is a valid sRGB encode", "[color][lut]") {
    // Empty proof + empty monitor = plain working→sRGB-display transform
    const auto lut = buildDisplayLut({}, ProofIntent::RelativeColorimetric, false, {});
    REQUIRE(lut.valid());

    // The LUT is indexed by sRGB-encoded working values and outputs
    // display-encoded values — with an sRGB display both ends use the same
    // encoding, so the diagonal should be near-identity. Check the corners.
    const int n = lut.size;
    auto at = [&](int r, int g, int b, int c) {
        return lut.data[((size_t(b) * n + g) * n + r) * 4 + c];
    };
    CHECK_THAT(at(0, 0, 0, 0), WithinAbs(0.0, 2e-2));
    CHECK_THAT(at(n - 1, n - 1, n - 1, 0), WithinAbs(1.0, 2e-2));
    CHECK_THAT(at(n - 1, n - 1, n - 1, 1), WithinAbs(1.0, 2e-2));
    // All in gamut (alpha = 1) when not proofing
    CHECK_THAT(at(n / 2, n / 2, n / 2, 3), WithinAbs(1.0, 1e-6));
}
