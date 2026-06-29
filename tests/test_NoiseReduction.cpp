#include "core/NoiseReduction.h"
#include "develop/GlobalAdjustment.h"
#include "io/XmpSidecar.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
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
// colorNoiseReductionStrengthMix — Strength→[0,1] blend factor for the recombine
// pass (the renderer uploads it as a uniform; the shader mixes raw vs blurred).
// ---------------------------------------------------------------------------

TEST_CASE("colorNoiseReductionStrengthMix maps Strength 0..100 to a 0..1 mix factor", "[nr]") {
    REQUIRE(colorNoiseReductionStrengthMix(0.0f) == 0.0f);
    REQUIRE_THAT(colorNoiseReductionStrengthMix(50.0f), WithinAbs(0.5f, 1e-6));
    REQUIRE(colorNoiseReductionStrengthMix(100.0f) == 1.0f);
}

TEST_CASE("colorNoiseReductionStrengthMix clamps out-of-range Strength to [0,1]", "[nr]") {
    REQUIRE(colorNoiseReductionStrengthMix(-10.0f) == 0.0f);
    REQUIRE(colorNoiseReductionStrengthMix(150.0f) == 1.0f);
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

TEST_CASE("colorNoiseReductionSmoothness survives an XMP round-trip", "[nr]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString raw = dir.filePath("test.CR3");

    GlobalAdjustment params;
    params.colorNoiseReductionSmoothness = 73.0f;
    REQUIRE(XmpSidecar::saveAdjustments(raw, params));

    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(raw);
    REQUIRE_THAT(loaded.colorNoiseReductionSmoothness, WithinAbs(73.0f, 1e-4));
}

TEST_CASE("colorNoiseReduction (Strength) defaults to 0 when absent from the sidecar", "[nr]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString raw = dir.filePath("test.CR3");
    GlobalAdjustment params; // colorNoiseReduction (Strength) defaults to 0
    REQUIRE(XmpSidecar::saveAdjustments(raw, params));
    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(raw);
    REQUIRE_THAT(loaded.colorNoiseReduction, WithinAbs(0.0f, 1e-6));
}

TEST_CASE("colorNoiseReductionSmoothness defaults to 50 in a pre-split sidecar", "[nr]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString raw = dir.filePath("test.CR3");

    // A sidecar written before the Amount→Smoothness/Strength split (issue #59)
    // carries crs:ColorNoiseReduction but no crs:ColorNoiseReductionSmoothness.
    GlobalAdjustment params;
    params.colorNoiseReduction = 30.0f; // the old single value, now read as Strength
    REQUIRE(XmpSidecar::saveAdjustments(raw, params));

    const QString sidecar = QDir(dir.path()).entryList({"*.xmp"}, QDir::Files).value(0);
    REQUIRE(!sidecar.isEmpty());
    QFile f(dir.filePath(sidecar));
    REQUIRE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QString xml = QString::fromUtf8(f.readAll());
    f.close();
    static const QRegularExpression re(R"(\s*crs:ColorNoiseReductionSmoothness="[^"]*")");
    xml.remove(re);
    REQUIRE_FALSE(xml.contains("ColorNoiseReductionSmoothness")); // absence truly simulated
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    f.write(xml.toUtf8());
    f.close();

    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(raw);
    REQUIRE_THAT(loaded.colorNoiseReductionSmoothness, WithinAbs(50.0f, 1e-6));
}
