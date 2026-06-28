#include "develop/DemosaicAlgorithm.h"

#include "develop/GlobalAdjustment.h"
#include "io/XmpSidecar.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

// ---------------------------------------------------------------------------
// librawUserQual — the DemosaicAlgorithm → libraw user_qual integer mapping.
// This is the single source of truth the decode reads; the documented integers
// (ADR 0036) must never drift.
// ---------------------------------------------------------------------------

TEST_CASE("librawUserQual maps each algorithm to its documented user_qual", "[demosaic]") {
    CHECK(librawUserQual(DemosaicAlgorithm::AHD) == 3);
    CHECK(librawUserQual(DemosaicAlgorithm::VNG) == 1);
    CHECK(librawUserQual(DemosaicAlgorithm::PPG) == 2);
    CHECK(librawUserQual(DemosaicAlgorithm::DCB) == 4);
    CHECK(librawUserQual(DemosaicAlgorithm::DHT) == 11);
    CHECK(librawUserQual(DemosaicAlgorithm::AAHD) == 12);
    CHECK(librawUserQual(DemosaicAlgorithm::Linear) == 0);
}

// ---------------------------------------------------------------------------
// demosaicToken / demosaicFromToken — the stable string persisted to the sidecar.
// ---------------------------------------------------------------------------

namespace {
constexpr DemosaicAlgorithm kAllAlgorithms[] = {
    DemosaicAlgorithm::AHD, DemosaicAlgorithm::VNG,  DemosaicAlgorithm::PPG,
    DemosaicAlgorithm::DCB, DemosaicAlgorithm::DHT,  DemosaicAlgorithm::AAHD,
    DemosaicAlgorithm::Linear,
};
}

TEST_CASE("demosaicToken / demosaicFromToken round-trips every enumerator", "[demosaic]") {
    for (const DemosaicAlgorithm algo : kAllAlgorithms) {
        const QString token = demosaicToken(algo);
        CHECK_FALSE(token.isEmpty());
        CHECK(demosaicFromToken(token) == algo);
    }
}

TEST_CASE("demosaicFromToken falls back silently to AHD for unusable tokens", "[demosaic]") {
    // The re-derive-don't-error contract (ADR 0021): an absent property, a
    // deferred/out-of-scope algorithm, or corruption all resolve to the default.
    CHECK(demosaicFromToken("") == kDefaultDemosaic);
    CHECK(demosaicFromToken("AMaZE") == kDefaultDemosaic); // GPL pack, not built
    CHECK(demosaicFromToken("RCD") == kDefaultDemosaic);   // not in libraw
    CHECK(demosaicFromToken("ahd") == kDefaultDemosaic);   // case-sensitive token
    CHECK(demosaicFromToken("\x01garbage") == kDefaultDemosaic);
    CHECK(kDefaultDemosaic == DemosaicAlgorithm::AHD);
}

// ---------------------------------------------------------------------------
// sensorSupportsDemosaicSelection — the Bayer-only gate (ADR 0036). Driven by
// libraw's imgdata.idata.filters; the UI disables the control when it is false.
// ---------------------------------------------------------------------------

TEST_CASE("only Bayer mosaics support demosaic selection", "[demosaic]") {
    // Standard Bayer CFAs (the common RGGB packing and friends) report non-zero,
    // non-9 filter masks.
    CHECK(sensorSupportsDemosaicSelection(0x94949494U)); // RGGB
    CHECK(sensorSupportsDemosaicSelection(0x61616161U)); // BGGR-style

    // X-Trans (9): libraw would silently reinterpret user_qual as Markesteijn.
    CHECK_FALSE(sensorSupportsDemosaicSelection(9U));
    // Foveon / already-demosaiced / linear / monochrome: no mosaic at all.
    CHECK_FALSE(sensorSupportsDemosaicSelection(0U));
}

// ---------------------------------------------------------------------------
// Persistence — arraw:DemosaicAlgorithm in the XMP sidecar.
// ---------------------------------------------------------------------------

TEST_CASE("a non-default demosaic algorithm round-trips through the sidecar", "[demosaic]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString raw = dir.filePath("IMG_0001.CR3");

    GlobalAdjustment params;
    params.demosaicAlgorithm = DemosaicAlgorithm::DCB;
    REQUIRE(XmpSidecar::saveAdjustments(raw, params));

    // The token, not libraw's integer, is what lands in the file.
    const QString sidecar = QDir(dir.path()).entryList({"*.xmp"}, QDir::Files).value(0);
    REQUIRE(!sidecar.isEmpty());
    QFile f(dir.filePath(sidecar));
    REQUIRE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString xml = QString::fromUtf8(f.readAll());
    CHECK(xml.contains("DemosaicAlgorithm=\"DCB\""));

    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(raw);
    CHECK(loaded.demosaicAlgorithm == DemosaicAlgorithm::DCB);
}

TEST_CASE("a sidecar without DemosaicAlgorithm resolves to AHD (legacy)", "[demosaic]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString raw = dir.filePath("IMG_0001.CR3");

    // Write a default sidecar, then strip the property to simulate a pre-feature
    // file (and confirm the default save omits it — AHD is the implicit default).
    GlobalAdjustment params; // demosaicAlgorithm defaults to AHD
    REQUIRE(XmpSidecar::saveAdjustments(raw, params));

    const QString sidecar = QDir(dir.path()).entryList({"*.xmp"}, QDir::Files).value(0);
    REQUIRE(!sidecar.isEmpty());
    QFile f(dir.filePath(sidecar));
    REQUIRE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QString xml = QString::fromUtf8(f.readAll());
    f.close();
    CHECK_FALSE(xml.contains("DemosaicAlgorithm")); // default AHD is not written

    static const QRegularExpression re(R"(\s*arraw:DemosaicAlgorithm="[^"]*")");
    xml.remove(re);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    f.write(xml.toUtf8());
    f.close();

    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(raw);
    CHECK(loaded.demosaicAlgorithm == DemosaicAlgorithm::AHD);
}
