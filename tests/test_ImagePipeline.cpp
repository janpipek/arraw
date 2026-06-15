#include "ImagePipeline.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <QFile>
#include <QRegularExpression>

using Catch::Matchers::WithinAbs;

// ── computeCurveLUT ───────────────────────────────────────────────────────────

TEST_CASE("identity curve produces identity LUT", "[curve]") {
    const auto lut = computeCurveLUT({{0.0, 0.0}, {1.0, 1.0}});
    for (int k = 0; k < 256; ++k)
        REQUIRE_THAT(lut[k], WithinAbs(k / 255.0, 1e-6));
}

TEST_CASE("LUT is flat outside the outermost control points", "[curve]") {
    const auto lut = computeCurveLUT({{0.2, 0.3}, {0.8, 0.9}});
    REQUIRE_THAT(lut[0], WithinAbs(0.3, 1e-6));   // below first point
    REQUIRE_THAT(lut[25], WithinAbs(0.3, 1e-6));  // 25/255 < 0.2
    REQUIRE_THAT(lut[255], WithinAbs(0.9, 1e-6)); // above last point
    REQUIRE_THAT(lut[230], WithinAbs(0.9, 1e-6)); // 230/255 > 0.8
}

TEST_CASE("LUT through monotone control points is monotone", "[curve]") {
    // A steep S-curve — naive Catmull-Rom would overshoot; Fritsch-Carlson
    // must not (that is the whole point of the tangent rescale).
    const auto lut = computeCurveLUT(
        {{0.0, 0.0}, {0.25, 0.05}, {0.5, 0.9}, {0.75, 0.95}, {1.0, 1.0}});
    for (int k = 1; k < 256; ++k)
        REQUIRE(lut[k] >= lut[k - 1] - 1e-7f);
}

TEST_CASE("LUT interpolates through the control points", "[curve]") {
    // Control points placed on the k/255 grid so the LUT samples them exactly.
    const auto lut = computeCurveLUT(
        {{0.0, 0.0}, {51 / 255.0, 0.25}, {204 / 255.0, 0.75}, {1.0, 1.0}});
    REQUIRE_THAT(lut[51], WithinAbs(0.25, 1e-6));
    REQUIRE_THAT(lut[204], WithinAbs(0.75, 1e-6));
}

TEST_CASE("LUT output is clamped to [0,1]", "[curve]") {
    const auto lut = computeCurveLUT({{0.0, -0.5}, {1.0, 1.5}});
    for (int k = 0; k < 256; ++k) {
        REQUIRE(lut[k] >= 0.0f);
        REQUIRE(lut[k] <= 1.0f);
    }
}

TEST_CASE("degenerate control points do not crash", "[curve]") {
    SECTION("empty input fills with zero") {
        const auto lut = computeCurveLUT({});
        for (int k = 0; k < 256; ++k)
            REQUIRE(lut[k] == 0.0f);
    }
    SECTION("single point fills with its y") {
        const auto lut = computeCurveLUT({{0.5, 0.7}});
        for (int k = 0; k < 256; ++k)
            REQUIRE_THAT(lut[k], WithinAbs(0.7, 1e-6));
    }
    SECTION("duplicate x values are deduplicated") {
        const auto lut = computeCurveLUT({{0.0, 0.0}, {0.5, 0.4}, {0.5, 0.6}, {1.0, 1.0}});
        for (int k = 0; k < 256; ++k) {
            REQUIRE(lut[k] >= 0.0f);
            REQUIRE(lut[k] <= 1.0f);
        }
    }
}

TEST_CASE("isIdentityCurve recognises only the default endpoints", "[curve]") {
    REQUIRE(isIdentityCurve({{0.0, 0.0}, {1.0, 1.0}}));
    REQUIRE_FALSE(isIdentityCurve({{0.0, 0.0}, {0.5, 0.5}, {1.0, 1.0}}));
    REQUIRE_FALSE(isIdentityCurve({{0.0, 0.1}, {1.0, 1.0}}));
}

// ── downsample2x ──────────────────────────────────────────────────────────────

namespace {
ImageBuffer makeBuffer(int w, int h, std::vector<float> data) {
    ImageBuffer b;
    b.width = w;
    b.height = h;
    b.data = std::move(data);
    return b;
}
} // namespace

TEST_CASE("downsample2x averages each 2x2 block exactly", "[downsample]") {
    // 2×2, one channel value per pixel position to expose index mistakes
    const auto src = makeBuffer(
        2,
        2,
        {
            0.0f,
            0.1f,
            0.2f,
            0.4f,
            0.5f,
            0.6f,
            0.8f,
            0.9f,
            1.0f,
            0.0f,
            0.5f,
            1.0f,
        });
    const auto dst = downsample2x(src);
    REQUIRE(dst.width == 1);
    REQUIRE(dst.height == 1);
    REQUIRE_THAT(dst.data[0], WithinAbs((0.0 + 0.4 + 0.8 + 0.0) / 4.0, 1e-7));
    REQUIRE_THAT(dst.data[1], WithinAbs((0.1 + 0.5 + 0.9 + 0.5) / 4.0, 1e-7));
    REQUIRE_THAT(dst.data[2], WithinAbs((0.2 + 0.6 + 1.0 + 1.0) / 4.0, 1e-7));
}

TEST_CASE("downsample2x halves dimensions, truncating odd sizes", "[downsample]") {
    const auto dst = downsample2x(makeBuffer(5, 3, std::vector<float>(5 * 3 * 3, 0.5f)));
    REQUIRE(dst.width == 2);
    REQUIRE(dst.height == 1);
    for (float v : dst.data)
        REQUIRE_THAT(v, WithinAbs(0.5, 1e-7));
}

TEST_CASE("downsample2x of invalid or sub-2x2 input is invalid", "[downsample]") {
    REQUIRE_FALSE(downsample2x({}).valid());
    REQUIRE_FALSE(downsample2x(makeBuffer(1, 1, {0.5f, 0.5f, 0.5f})).valid());
}

// ── shader constant consistency ───────────────────────────────────────────────

TEST_CASE("kLuma constants match shaders/image.frag", "[shader-consts]") {
    QFile frag(QStringLiteral(ARRAW_SOURCE_DIR "/shaders/image.frag"));
    REQUIRE(frag.open(QIODevice::ReadOnly));

    const QRegularExpression re(
        R"(kLuma\s*=\s*vec3\(\s*([0-9.]+)\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\))");
    const auto m = re.match(QString::fromUtf8(frag.readAll()));
    REQUIRE(m.hasMatch());

    REQUIRE_THAT(m.captured(1).toFloat(), WithinAbs(kLumaR, 1e-6));
    REQUIRE_THAT(m.captured(2).toFloat(), WithinAbs(kLumaG, 1e-6));
    REQUIRE_THAT(m.captured(3).toFloat(), WithinAbs(kLumaB, 1e-6));
}
