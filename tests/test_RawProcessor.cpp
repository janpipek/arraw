#include "RawProcessor.h"
#include "DemosaicAlgorithm.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>

// Structural decode tests against the tiny committed DNG (see
// fixtures/make_test_dng.py): a 32x24 linear-RGB horizontal gradient.
// Values are deliberately not pinned — libraw's color conversion and the
// exposure normalisation in RawProcessor make exact pixels an implementation
// detail; the golden-image phase covers appearance.

static const QString kDng = QStringLiteral(ARRAW_FIXTURE_DIR "/gradient-32x24.dng");

TEST_CASE("DNG fixture decodes to full-res plus half-res preview", "[raw][fixtures]") {
    const LoadResult r = RawProcessor::load(kDng);

    REQUIRE(r.error.isEmpty());
    REQUIRE(r.fullRes.valid());
    CHECK(r.fullRes.width == 32);
    CHECK(r.fullRes.height == 24);
    REQUIRE(r.preview.valid());
    CHECK(r.preview.width == r.fullRes.width / 2);
    CHECK(r.preview.height == r.fullRes.height / 2);
    CHECK(r.defaultCrop == QRectF(0.0, 0.0, 1.0, 1.0));

    for (float v : r.fullRes.data) {
        REQUIRE(std::isfinite(v));
        REQUIRE(v >= 0.0f);
    }
}

TEST_CASE("decoded gradient keeps left-to-right ordering", "[raw][fixtures]") {
    const LoadResult r = RawProcessor::load(kDng);
    REQUIRE(r.fullRes.valid());

    const auto& b = r.fullRes;
    auto meanLuma = [&](int x0, int x1) {
        double sum = 0.0;
        int n = 0;
        for (int y = 0; y < b.height; ++y)
            for (int x = x0; x < x1; ++x) {
                const float* p = b.data.data() + (size_t(y) * b.width + x) * 3;
                sum += p[0] * kLumaR + p[1] * kLumaG + p[2] * kLumaB;
                ++n;
            }
        return sum / n;
    };

    const double left = meanLuma(0, b.width / 4);
    const double right = meanLuma(b.width * 3 / 4, b.width);
    CHECK(right > left * 2.0);
}

TEST_CASE("decoding honours an explicit demosaic algorithm", "[raw][fixtures]") {
    // A non-default algorithm must thread through to libraw without breaking the
    // decode. (The gradient fixture is linear, so the algorithm cannot change
    // pixels here — the visual difference is verified by eye on real Bayer RAWs.)
    const LoadResult r = RawProcessor::load(kDng, nullptr, nullptr, DemosaicAlgorithm::VNG);
    REQUIRE(r.error.isEmpty());
    REQUIRE(r.fullRes.valid());
    CHECK(r.fullRes.width == 32);
    CHECK(r.fullRes.height == 24);
}

TEST_CASE("LoadResult surfaces the sensor mosaic for the disable gate", "[raw][fixtures]") {
    const LoadResult r = RawProcessor::load(kDng);
    REQUIRE(r.error.isEmpty());
    // The linear gradient DNG has no Bayer mosaic, so selection is unavailable.
    CHECK_FALSE(sensorSupportsDemosaicSelection(r.filters));
}

TEST_CASE("missing file reports an error, not a crash", "[raw]") {
    const LoadResult r = RawProcessor::load(QStringLiteral("/nonexistent/nope.dng"));
    CHECK_FALSE(r.error.isEmpty());
    CHECK_FALSE(r.fullRes.valid());
}

TEST_CASE("fixture without thumbnail yields no embedded preview", "[raw][fixtures]") {
    CHECK_FALSE(RawProcessor::loadEmbeddedPreview(kDng).valid());
}
