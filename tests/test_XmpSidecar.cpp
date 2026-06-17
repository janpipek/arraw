#include "ImagePipeline.h"
#include "XmpSidecar.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <QFile>
#include <QTemporaryDir>

using Catch::Matchers::WithinAbs;

namespace {

// Scalars are written with 4 decimals; curve points are quantised to the
// 0..255 integer grid. These tolerances mirror that, not float noise.
constexpr double kScalarTol = 1e-4;
constexpr double kCurveTol = 0.5 / 255.0;

GlobalAdjustment sampleParams() {
    GlobalAdjustment p;
    p.exposure = 1.25f;
    p.contrast = -30.0f;
    p.highlights = -55.5f;
    p.shadows = 42.0f;
    p.whites = 7.0f;
    p.blacks = -3.25f;
    p.temperature = 7200.0f;
    p.tint = -12.0f;
    p.saturation = 15.0f;
    p.vibrance = 33.0f;
    p.sharpening = 40.0f;
    p.rotation = -2.5f;
    p.cropRect = QRectF(0.1, 0.2, 0.75, 0.6);
    for (int i = 0; i < 8; ++i) {
        p.hslHue[i] = float(i * 10 - 40);
        p.hslSat[i] = float(40 - i * 10);
        p.hslLum[i] = float(i % 2 ? 25 : -25);
    }
    // Points on the 0..255 grid so quantisation is lossless
    p.curveLuma.points = {{0.0, 0.0}, {64 / 255.0, 32 / 255.0}, {1.0, 1.0}};
    p.curveR.points = {{0.0, 16 / 255.0}, {1.0, 240 / 255.0}};
    return p;
}

void checkClose(const GlobalAdjustment& a, const GlobalAdjustment& b) {
    CHECK_THAT(a.exposure, WithinAbs(b.exposure, kScalarTol));
    CHECK_THAT(a.contrast, WithinAbs(b.contrast, kScalarTol));
    CHECK_THAT(a.highlights, WithinAbs(b.highlights, kScalarTol));
    CHECK_THAT(a.shadows, WithinAbs(b.shadows, kScalarTol));
    CHECK_THAT(a.whites, WithinAbs(b.whites, kScalarTol));
    CHECK_THAT(a.blacks, WithinAbs(b.blacks, kScalarTol));
    CHECK_THAT(a.temperature, WithinAbs(b.temperature, kScalarTol));
    CHECK_THAT(a.tint, WithinAbs(b.tint, kScalarTol));
    CHECK_THAT(a.saturation, WithinAbs(b.saturation, kScalarTol));
    CHECK_THAT(a.vibrance, WithinAbs(b.vibrance, kScalarTol));
    CHECK_THAT(a.sharpening, WithinAbs(b.sharpening, kScalarTol));
    CHECK_THAT(a.rotation, WithinAbs(b.rotation, kScalarTol));
    CHECK_THAT(a.cropRect.left(), WithinAbs(b.cropRect.left(), kScalarTol));
    CHECK_THAT(a.cropRect.top(), WithinAbs(b.cropRect.top(), kScalarTol));
    CHECK_THAT(a.cropRect.right(), WithinAbs(b.cropRect.right(), 2 * kScalarTol));
    CHECK_THAT(a.cropRect.bottom(), WithinAbs(b.cropRect.bottom(), 2 * kScalarTol));
    for (int i = 0; i < 8; ++i) {
        CHECK_THAT(a.hslHue[i], WithinAbs(b.hslHue[i], kScalarTol));
        CHECK_THAT(a.hslSat[i], WithinAbs(b.hslSat[i], kScalarTol));
        CHECK_THAT(a.hslLum[i], WithinAbs(b.hslLum[i], kScalarTol));
    }
}

void checkCurveClose(const CurvePoints& a, const CurvePoints& b) {
    REQUIRE(a.points.size() == b.points.size());
    for (size_t i = 0; i < a.points.size(); ++i) {
        CHECK_THAT(a.points[i].x(), WithinAbs(b.points[i].x(), kCurveTol));
        CHECK_THAT(a.points[i].y(), WithinAbs(b.points[i].y(), kCurveTol));
    }
}

} // namespace

TEST_CASE("sidecar path replaces the RAW extension with .xmp", "[xmp]") {
    REQUIRE(XmpSidecar::pathFor("/photos/IMG_0042.ARW") == "/photos/IMG_0042.xmp");
    REQUIRE(XmpSidecar::pathFor("/photos/IMG_0042.dng") == "/photos/IMG_0042.xmp");
}

TEST_CASE("missing sidecar loads default params", "[xmp]") {
    QTemporaryDir dir;
    REQUIRE(XmpSidecar::loadAdjustments(dir.filePath("nothing-here.arw")) == GlobalAdjustment{});
}

TEST_CASE("resolveAdjustments returns the sidecar params when present, ignoring defaultCrop",
          "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("shot.arw");
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, sampleParams()));

    // A defaultCrop that must NOT win — the sidecar's own crop should be kept.
    const QRectF defaultCrop(0.0, 0.0, 0.123, 0.456);
    const auto resolved = XmpSidecar::resolveAdjustments(rawPath, defaultCrop);

    checkClose(resolved, sampleParams()); // crop comes from the sidecar, not defaultCrop
}

TEST_CASE("resolveAdjustments falls back to defaults with defaultCrop when no sidecar",
          "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("never-edited.arw"); // no .xmp written

    const QRectF defaultCrop(0.1, 0.2, 0.7, 0.6);
    const auto resolved = XmpSidecar::resolveAdjustments(rawPath, defaultCrop);

    GlobalAdjustment expected; // defaults
    expected.cropRect = defaultCrop;
    REQUIRE(resolved == expected);
}

TEST_CASE("save then load round-trips all params", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("shot.arw");
    const auto saved = sampleParams();

    REQUIRE(XmpSidecar::saveAdjustments(rawPath, saved));
    const auto loaded = XmpSidecar::loadAdjustments(rawPath);

    checkClose(loaded, saved);
    checkCurveClose(loaded.curveLuma, saved.curveLuma);
    checkCurveClose(loaded.curveR, saved.curveR);
    CHECK(loaded.curveG.isIdentity()); // identity curves are not written
    CHECK(loaded.curveB.isIdentity());
}

TEST_CASE("local adjustments round-trip through the arraw namespace", "[xmp][arraw]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("local.dng");

    GlobalAdjustment p;
    LocalAdjustment la;
    la.mask = LinearMask{{0.2, 0.5}, {0.8, 0.5}};
    la.exposure = 0.5f;
    la.contrast = -10.0f;
    la.temperature = 25.0f; // relative -100..100, not Kelvin
    p.localAdjustments.push_back(la);

    REQUIRE(XmpSidecar::saveAdjustments(rawPath, p));
    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(rawPath);

    REQUIRE(loaded.localAdjustments.size() == 1);
    const LocalAdjustment& r = loaded.localAdjustments[0];
    REQUIRE(std::holds_alternative<LinearMask>(r.mask));
    const LinearMask& m = std::get<LinearMask>(r.mask);
    CHECK_THAT(m.p0.x(), WithinAbs(0.2, kScalarTol));
    CHECK_THAT(m.p0.y(), WithinAbs(0.5, kScalarTol));
    CHECK_THAT(m.p1.x(), WithinAbs(0.8, kScalarTol));
    CHECK_THAT(m.p1.y(), WithinAbs(0.5, kScalarTol));
    CHECK_THAT(r.exposure, WithinAbs(0.5, kScalarTol));
    CHECK_THAT(r.contrast, WithinAbs(-10.0, kScalarTol));
    CHECK_THAT(r.temperature, WithinAbs(25.0, kScalarTol));
}

// The arraw-native format contract — exact emitted fields, independent of our
// reader. A matched reader/writer bug passes round-trip but fails this.
TEST_CASE(
    "writer emits local adjustments in the arraw namespace with relative "
    "temperature",
    "[xmp][arraw]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("shot.arw");

    GlobalAdjustment p;
    LocalAdjustment la;
    la.mask = LinearMask{{0.2, 0.5}, {0.8, 0.5}};
    la.exposure = 0.5f;
    la.temperature = 25.0f; // relative, NOT Kelvin
    p.localAdjustments.push_back(la);
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, p));

    QFile f(XmpSidecar::pathFor(rawPath));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString xml = QString::fromUtf8(f.readAll());

    CHECK(xml.contains("http://ns.arraw.app/1.0/"));
    CHECK(xml.contains("<arraw:MaskType>Linear</arraw:MaskType>"));
    CHECK(xml.contains("<arraw:P0x>0.2000</arraw:P0x>"));
    CHECK(xml.contains("<arraw:Exposure>0.5000</arraw:Exposure>"));
    CHECK(xml.contains("<arraw:Temperature>25.0000</arraw:Temperature>"));
    // It must NOT have leaked into the Lightroom crs: develop block.
    CHECK_FALSE(xml.contains(R"(crs:LocalAdjustments)"));
}

TEST_CASE("radial local adjustments round-trip through the arraw namespace", "[xmp][arraw]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("radial.dng");

    GlobalAdjustment p;
    LocalAdjustment la;
    la.mask = RadialMask{{0.4, 0.6}, 0.3, 0.2, 30.0, 0.4, true};
    la.exposure = -0.5f;
    p.localAdjustments.push_back(la);

    REQUIRE(XmpSidecar::saveAdjustments(rawPath, p));
    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(rawPath);

    REQUIRE(loaded.localAdjustments.size() == 1);
    const LocalAdjustment& r = loaded.localAdjustments[0];
    REQUIRE(std::holds_alternative<RadialMask>(r.mask));
    const RadialMask& m = std::get<RadialMask>(r.mask);
    CHECK_THAT(m.center.x(), WithinAbs(0.4, kScalarTol));
    CHECK_THAT(m.center.y(), WithinAbs(0.6, kScalarTol));
    CHECK_THAT(m.radiusX, WithinAbs(0.3, kScalarTol));
    CHECK_THAT(m.radiusY, WithinAbs(0.2, kScalarTol));
    CHECK_THAT(m.angle, WithinAbs(30.0, kScalarTol));
    CHECK_THAT(m.feather, WithinAbs(0.4, kScalarTol));
    CHECK(m.invert);
    CHECK_THAT(r.exposure, WithinAbs(-0.5, kScalarTol));
}

TEST_CASE("loading drops local adjustments beyond the 16-mask cap", "[xmp][arraw]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("many.dng");

    GlobalAdjustment p;
    for (int i = 0; i < 20; ++i) {
        LocalAdjustment la;
        la.mask = LinearMask{{0.0, 0.0}, {1.0, 1.0}};
        la.exposure = float(i);
        p.localAdjustments.push_back(la);
    }
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, p));

    REQUIRE(XmpSidecar::loadAdjustments(rawPath).localAdjustments.size() == 16);
}

TEST_CASE("crop aspect-lock flag round-trips via crs:CropConstrainAspectRatio", "[xmp][crs]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("shot.arw");

    GlobalAdjustment locked;
    locked.cropConstrained = true;
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, locked));
    REQUIRE(XmpSidecar::loadAdjustments(rawPath).cropConstrained);

    GlobalAdjustment unlocked; // default: not constrained
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, unlocked));
    REQUIRE_FALSE(XmpSidecar::loadAdjustments(rawPath).cropConstrained);
}

TEST_CASE("default params round-trip to defaults", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("untouched.nef");
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, {}));
    checkClose(XmpSidecar::loadAdjustments(rawPath), {});
}

// The crs: contract (Lightroom compatibility) — exact emitted fields, not
// just self-consistency. A matched reader/writer bug passes round-trip
// tests but fails these.
TEST_CASE("writer emits crs:Temperature in absolute Kelvin", "[xmp][crs]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("shot.arw");
    GlobalAdjustment p;
    p.temperature = 6500.0f;
    p.exposure = 0.85f;
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, p));

    QFile f(XmpSidecar::pathFor(rawPath));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString xml = QString::fromUtf8(f.readAll());

    CHECK(xml.contains(R"(crs:Temperature="6500.0000")"));
    CHECK(xml.contains(R"(crs:Exposure2012="0.8500")"));
    CHECK(xml.contains("http://ns.adobe.com/camera-raw-settings/1.0/"));
}

TEST_CASE("reader parses a Lightroom-style sidecar", "[xmp][crs]") {
    // Fixture mimics Adobe output: signed values with leading '+', Temperature
    // in Kelvin, crop as normalised edges, tone curve as 0..255 rdf:Seq.
    const auto p = XmpSidecar::loadAdjustments(
        QStringLiteral(ARRAW_FIXTURE_DIR "/lightroom-sample.arw"));

    CHECK_THAT(p.exposure, WithinAbs(0.85, 1e-5));
    CHECK_THAT(p.contrast, WithinAbs(12.0, 1e-5));
    CHECK_THAT(p.highlights, WithinAbs(-40.0, 1e-5));
    CHECK_THAT(p.shadows, WithinAbs(35.0, 1e-5));
    CHECK_THAT(p.temperature, WithinAbs(6500.0, 1e-5)); // Kelvin, the contract
    CHECK_THAT(p.tint, WithinAbs(10.0, 1e-5));
    CHECK_THAT(p.vibrance, WithinAbs(25.0, 1e-5));
    CHECK_THAT(p.hslSat[5], WithinAbs(-15.0, 1e-5)); // SaturationAdjustmentBlue

    // CropLeft/Top/Right/Bottom 0.05/0.1/0.95/0.9 → x/y/w/h
    CHECK_THAT(p.cropRect.left(), WithinAbs(0.05, 1e-5));
    CHECK_THAT(p.cropRect.top(), WithinAbs(0.10, 1e-5));
    CHECK_THAT(p.cropRect.width(), WithinAbs(0.90, 1e-5));
    CHECK_THAT(p.cropRect.height(), WithinAbs(0.80, 1e-5));

    // ToneCurvePV2012: (0,0) (64,48) (255,255) on the 0..255 grid
    REQUIRE(p.curveLuma.points.size() == 3);
    CHECK_THAT(p.curveLuma.points[1].x(), WithinAbs(64 / 255.0, 1e-5));
    CHECK_THAT(p.curveLuma.points[1].y(), WithinAbs(48 / 255.0, 1e-5));
    CHECK(p.curveR.isIdentity());
}

// ── User metadata: rating + colour label ────────────────────────────────────

TEST_CASE("ColourLabel maps to and from the canonical English name", "[xmp][marks]") {
    CHECK(colourLabelToString(ColourLabel::Green) == "Green");
    CHECK(colourLabelToString(ColourLabel::None).isEmpty());
    CHECK(colourLabelFromString("Purple") == ColourLabel::Purple);
    CHECK(colourLabelFromString("") == ColourLabel::None);
    CHECK(colourLabelFromString("Chartreuse") == ColourLabel::None); // unknown → None
}

TEST_CASE("missing sidecar loads default marks", "[xmp][marks]") {
    QTemporaryDir dir;
    CHECK(XmpSidecar::loadMetadata(dir.filePath("nothing-here.arw")) == UserMetadata{});
}

TEST_CASE("rating and label round-trip", "[xmp][marks]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("shot.arw");

    REQUIRE(XmpSidecar::saveMetadata(rawPath, {4, ColourLabel::Blue}));
    CHECK(XmpSidecar::loadMetadata(rawPath) == UserMetadata{4, ColourLabel::Blue});
}

TEST_CASE("reject is stored as rating -1", "[xmp][marks]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("reject.arw");

    REQUIRE(XmpSidecar::saveMetadata(rawPath, {-1, ColourLabel::None}));
    CHECK(XmpSidecar::loadMetadata(rawPath).rating == -1);

    QFile f(XmpSidecar::pathFor(rawPath));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString xml = QString::fromUtf8(f.readAll());
    CHECK(xml.contains(R"(xmp:Rating="-1")"));
    CHECK(xml.contains("http://ns.adobe.com/xap/1.0/"));
}

TEST_CASE("default marks are not written to the sidecar", "[xmp][marks]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("clean.arw");

    REQUIRE(XmpSidecar::saveMetadata(rawPath, UserMetadata{}));

    QFile f(XmpSidecar::pathFor(rawPath));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString xml = QString::fromUtf8(f.readAll());
    CHECK_FALSE(xml.contains("xmp:Rating"));
    CHECK_FALSE(xml.contains("xmp:Label"));
}

// The clobber guarantee (docs/adr/0007): the two namespace-scoped saves are
// read-first, so neither destroys the half it doesn't own.
TEST_CASE("saveMetadata preserves existing develop edits", "[xmp][marks]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("edited.arw");

    const auto edits = sampleParams();
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, edits));
    REQUIRE(XmpSidecar::saveMetadata(rawPath, {5, ColourLabel::Red}));

    CHECK(XmpSidecar::loadMetadata(rawPath) == UserMetadata{5, ColourLabel::Red});
    checkClose(XmpSidecar::loadAdjustments(rawPath), edits); // edits survived
}

TEST_CASE("saveAdjustments preserves existing marks", "[xmp][marks]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("rated.arw");

    REQUIRE(XmpSidecar::saveMetadata(rawPath, {3, ColourLabel::Yellow}));
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, sampleParams()));

    CHECK(
        XmpSidecar::loadMetadata(rawPath) == UserMetadata{3, ColourLabel::Yellow}); // marks survived
    checkClose(XmpSidecar::loadAdjustments(rawPath), sampleParams());
}
