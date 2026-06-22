#include "ImagePipeline.h"
#include "NoiseReduction.h"
#include "XmpSidecar.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <QTemporaryDir>

using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// colorNoiseReductionSigmaPx — the Amount→sigma calibration the renderer uploads
// ---------------------------------------------------------------------------

TEST_CASE("colorNoiseReductionSigmaPx maps Amount 0 to sigma 0 (NR off)", "[nr]") {
    REQUIRE(colorNoiseReductionSigmaPx(0.0f) == 0.0f);
}

TEST_CASE("colorNoiseReductionSigmaPx increases monotonically to ~25px at 100", "[nr]") {
    const float lo = colorNoiseReductionSigmaPx(25.0f);
    const float mid = colorNoiseReductionSigmaPx(50.0f);
    const float hi = colorNoiseReductionSigmaPx(100.0f);
    REQUIRE(lo > 0.0f);
    REQUIRE(mid > lo);
    REQUIRE(hi > mid);
    REQUIRE_THAT(hi, WithinAbs(25.0f, 1.0f));
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

TEST_CASE("colorNoiseReduction survives an XMP round-trip", "[nr]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString raw = dir.filePath("test.CR3");

    GlobalAdjustment params;
    params.colorNoiseReduction = 42.5f;
    REQUIRE(XmpSidecar::saveAdjustments(raw, params));

    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(raw);
    REQUIRE_THAT(loaded.colorNoiseReduction, WithinAbs(42.5f, 1e-4));
}

TEST_CASE("colorNoiseReduction defaults to 0 when absent from the sidecar", "[nr]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString raw = dir.filePath("test.CR3");
    GlobalAdjustment params; // colorNoiseReduction defaults to 0
    REQUIRE(XmpSidecar::saveAdjustments(raw, params));
    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(raw);
    REQUIRE_THAT(loaded.colorNoiseReduction, WithinAbs(0.0f, 1e-6));
}
