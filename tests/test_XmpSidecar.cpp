#include "core/Orientation.h"
#include "develop/LocalAdjustment.h"
#include "develop/Spot.h"
#include "develop/UserMetadata.h"
#include "develop/GlobalAdjustment.h"
#include "develop/Snapshot.h"
#include "io/XmpSidecar.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <QDomDocument>
#include <QFile>
#include <QTemporaryDir>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

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
    p.filmicHighlights = 65.0f;
    p.temperature = 7200.0f;
    p.tint = -12.0f;
    p.saturation = 15.0f;
    p.vibrance = 33.0f;
    p.sharpening = 40.0f;
    p.rotation = -2.5f;
    p.cropRect = QRectF(0.1, 0.2, 0.75, 0.6);
    p.postCropVignetteAmount = -28.0f;
    p.postCropVignetteMidpoint = 61.0f;
    p.postCropVignetteFeather = 73.0f;
    p.grainAmount = 32.0f;
    p.grainSize = 44.0f;
    p.grainRoughness = 79.0f;
    p.grainSeed = 3735928559U;
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
    CHECK_THAT(a.filmicHighlights, WithinAbs(b.filmicHighlights, kScalarTol));
    CHECK_THAT(a.temperature, WithinAbs(b.temperature, kScalarTol));
    CHECK_THAT(a.tint, WithinAbs(b.tint, kScalarTol));
    CHECK_THAT(a.saturation, WithinAbs(b.saturation, kScalarTol));
    CHECK_THAT(a.vibrance, WithinAbs(b.vibrance, kScalarTol));
    CHECK_THAT(a.sharpening, WithinAbs(b.sharpening, kScalarTol));
    CHECK_THAT(a.rotation, WithinAbs(b.rotation, kScalarTol));
    CHECK_THAT(a.postCropVignetteAmount, WithinAbs(b.postCropVignetteAmount, kScalarTol));
    CHECK_THAT(a.postCropVignetteMidpoint, WithinAbs(b.postCropVignetteMidpoint, kScalarTol));
    CHECK_THAT(a.postCropVignetteFeather, WithinAbs(b.postCropVignetteFeather, kScalarTol));
    CHECK_THAT(a.grainAmount, WithinAbs(b.grainAmount, kScalarTol));
    CHECK_THAT(a.grainSize, WithinAbs(b.grainSize, kScalarTol));
    CHECK_THAT(a.grainRoughness, WithinAbs(b.grainRoughness, kScalarTol));
    CHECK(a.grainSeed == b.grainSeed);
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

TEST_CASE("orientation round-trips through the sidecar as tiff:Orientation", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("IMG_0001.arw");
    GlobalAdjustment p;
    p.orientation = orient::Orientation{1, true}; // a turned-and-mirrored state (EXIF 5)
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, p));
    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(rawPath);
    REQUIRE(loaded.orientation == p.orientation);
}

TEST_CASE("orientation seeds from EXIF when the sidecar has none", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("nosidecar.arw"); // no .xmp on disk
    const orient::Orientation seed{1, false};
    const GlobalAdjustment p = XmpSidecar::resolveAdjustments(rawPath, QRectF(0, 0, 1, 1), seed);
    REQUIRE(p.orientation == seed);
}

TEST_CASE("a stored orientation wins over the EXIF seed", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("stored.arw");
    GlobalAdjustment saved;
    saved.orientation = orient::Orientation{2, false}; // 180°, explicitly stored
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, saved));
    // Resolve with a *different* seed — the stored value must win.
    const GlobalAdjustment p
        = XmpSidecar::resolveAdjustments(rawPath, QRectF(0, 0, 1, 1), orient::Orientation{1, true});
    REQUIRE(p.orientation == saved.orientation);
}

TEST_CASE("sidecar path replaces the RAW extension with .xmp", "[xmp]") {
    REQUIRE(XmpSidecar::pathFor("/photos/IMG_0042.ARW") == "/photos/IMG_0042.xmp");
    REQUIRE(XmpSidecar::pathFor("/photos/IMG_0042.dng") == "/photos/IMG_0042.xmp");
}

TEST_CASE("an existing extension-specific sidecar is used", "[xmp][compatibility]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("catalogued.nef");
    const QString sidecarPath = rawPath + ".xmp";
    QFile sidecar(sidecarPath);
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    REQUIRE(
        sidecar.write(R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about="" xmlns:xmp="http://ns.adobe.com/xap/1.0/"
      xmp:Rating="2"/>
  </rdf:RDF>
</x:xmpmeta>)xml")
        > 0);
    sidecar.close();

    CHECK(XmpSidecar::pathFor(rawPath) == sidecarPath);
    CHECK(XmpSidecar::loadMetadata(rawPath).rating == 2);
    REQUIRE(XmpSidecar::saveMetadata(rawPath, {5, ColourLabel::Green}));
    CHECK(XmpSidecar::loadMetadata(rawPath) == UserMetadata{5, ColourLabel::Green});
    CHECK_FALSE(QFile::exists(dir.filePath("catalogued.xmp")));
}

TEST_CASE("loadAdjustments reads an extension-specific sidecar", "[xmp][compatibility]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("catalogued.nef");
    QFile sidecar(rawPath + ".xmp");
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    REQUIRE(
        sidecar.write(R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about=""
      xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
      crs:Exposure2012="0.8500"/>
  </rdf:RDF>
</x:xmpmeta>)xml")
        > 0);
    sidecar.close();

    CHECK_THAT(XmpSidecar::loadAdjustments(rawPath).exposure, WithinAbs(0.85, 1e-5));
}

TEST_CASE("a save with no existing sidecar creates the stem-named file", "[xmp][compatibility]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("fresh.nef");

    REQUIRE(XmpSidecar::saveAdjustments(rawPath, sampleParams()));

    CHECK(QFile::exists(dir.filePath("fresh.xmp")));
    CHECK_FALSE(QFile::exists(rawPath + ".xmp"));
}

TEST_CASE("two sidecar naming variants are ambiguous", "[xmp][compatibility]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("ambiguous.nef");
    const QString stemPath = dir.filePath("ambiguous.xmp");
    const QString extensionPath = rawPath + ".xmp";
    const QByteArray stemPacket = R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about="" xmlns:xmp="http://ns.adobe.com/xap/1.0/"
      xmp:Rating="1"/>
  </rdf:RDF>
</x:xmpmeta>)xml";
    const QByteArray extensionPacket = R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about="" xmlns:xmp="http://ns.adobe.com/xap/1.0/"
      xmp:Rating="2"/>
  </rdf:RDF>
</x:xmpmeta>)xml";
    QFile stem(stemPath);
    REQUIRE(stem.open(QIODevice::WriteOnly));
    REQUIRE(stem.write(stemPacket) == stemPacket.size());
    stem.close();
    QFile extension(extensionPath);
    REQUIRE(extension.open(QIODevice::WriteOnly));
    REQUIRE(extension.write(extensionPacket) == extensionPacket.size());
    extension.close();

    CHECK(XmpSidecar::loadWithStatus(rawPath).status == SidecarLoadStatus::ParseError);
    CHECK_FALSE(XmpSidecar::saveMetadata(rawPath, {5, ColourLabel::Purple}));
    CHECK_FALSE(XmpSidecar::saveAdjustments(rawPath, sampleParams()));

    REQUIRE(stem.open(QIODevice::ReadOnly));
    CHECK(stem.readAll() == stemPacket);
    REQUIRE(extension.open(QIODevice::ReadOnly));
    CHECK(extension.readAll() == extensionPacket);
}

TEST_CASE("missing sidecar loads default params", "[xmp]") {
    QTemporaryDir dir;
    REQUIRE(XmpSidecar::loadAdjustments(dir.filePath("nothing-here.arw")) == GlobalAdjustment{});
    CHECK(
        XmpSidecar::loadWithStatus(dir.filePath("nothing-here.arw")).status
        == SidecarLoadStatus::Missing);
}

TEST_CASE("malformed sidecar loads default params", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("broken.arw");
    QFile f(XmpSidecar::pathFor(rawPath));
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write("<x:xmpmeta><rdf:RDF><rdf:Description crs:Exposure2012=\"2.0\"") > 0);
    f.close();

    REQUIRE(XmpSidecar::loadAdjustments(rawPath) == GlobalAdjustment{});
    CHECK(XmpSidecar::loadWithStatus(rawPath).status == SidecarLoadStatus::ParseError);
}

TEST_CASE("saves never replace an existing malformed sidecar", "[xmp][compatibility]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("broken.nef");
    const QByteArray malformed = "<x:xmpmeta><rdf:RDF><rdf:Description crs:Exposure2012=\"2.0\"";
    QFile sidecar(XmpSidecar::pathFor(rawPath));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    REQUIRE(sidecar.write(malformed) == malformed.size());
    sidecar.close();

    CHECK_FALSE(XmpSidecar::saveMetadata(rawPath, {5, ColourLabel::Purple}));
    CHECK_FALSE(XmpSidecar::saveAdjustments(rawPath, sampleParams()));

    REQUIRE(sidecar.open(QIODevice::ReadOnly));
    CHECK(sidecar.readAll() == malformed);
}

TEST_CASE("a failed save preserves the original sidecar atomically", "[xmp][compatibility]") {
#ifdef Q_OS_WIN
    SKIP("read-only directory does not reliably block QSaveFile on Windows");
#else
    if (::geteuid() == 0)
        SKIP("root bypasses directory write permissions");

    QTemporaryDir dir;
    const QString rawPath = dir.filePath("locked.nef");
    const QByteArray original = R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about=""
      xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
      crs:Exposure2012="2.0000"/>
  </rdf:RDF>
</x:xmpmeta>)xml";
    QFile sidecar(XmpSidecar::pathFor(rawPath));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    REQUIRE(sidecar.write(original) == original.size());
    sidecar.close();

    // Strip write permission from the directory so QSaveFile cannot create or
    // rename its temporary file. QSaveFile does not fall back to a direct
    // write, so a doomed save must leave the original bytes untouched.
    REQUIRE(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const bool saved = XmpSidecar::saveAdjustments(rawPath, sampleParams());
    // Restore permissions so QTemporaryDir can clean up regardless of outcome.
    QFile::setPermissions(
        dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    CHECK_FALSE(saved);
    REQUIRE(sidecar.open(QIODevice::ReadOnly));
    CHECK(sidecar.readAll() == original);
#endif
}

TEST_CASE(
    "resolveAdjustments returns the sidecar params when present, ignoring defaultCrop", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("shot.arw");
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, sampleParams()));

    // A defaultCrop that must NOT win — the sidecar's own crop should be kept.
    const QRectF defaultCrop(0.0, 0.0, 0.123, 0.456);
    const auto resolved = XmpSidecar::resolveAdjustments(rawPath, defaultCrop);
    const auto resolvedWithStatus = XmpSidecar::resolveAdjustmentsWithStatus(rawPath, defaultCrop);

    checkClose(resolved, sampleParams()); // crop comes from the sidecar, not defaultCrop
    CHECK(resolvedWithStatus.status == SidecarLoadStatus::Loaded);
    checkClose(resolvedWithStatus.adjustments, sampleParams());
}

TEST_CASE("resolveForImage returns sidecar adjustments and metadata when present", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("shot.arw");
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, sampleParams()));
    REQUIRE(XmpSidecar::saveMetadata(rawPath, {4, ColourLabel::Purple}));

    const QRectF defaultCrop(0.0, 0.0, 0.123, 0.456);
    const auto resolved = XmpSidecar::resolveForImage(rawPath, defaultCrop);

    CHECK(resolved.status == SidecarLoadStatus::Loaded);
    checkClose(resolved.data.adjustments, sampleParams());
    CHECK(resolved.data.metadata == UserMetadata{4, ColourLabel::Purple});
}

TEST_CASE("resolveAdjustments falls back to defaults with defaultCrop when no sidecar", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("never-edited.arw"); // no .xmp written

    const QRectF defaultCrop(0.1, 0.2, 0.7, 0.6);
    const auto resolved = XmpSidecar::resolveAdjustments(rawPath, defaultCrop);
    const auto resolvedWithStatus = XmpSidecar::resolveAdjustmentsWithStatus(rawPath, defaultCrop);

    GlobalAdjustment expected; // defaults
    expected.cropRect = defaultCrop;
    REQUIRE(resolved == expected);
    CHECK(resolvedWithStatus.status == SidecarLoadStatus::Missing);
    REQUIRE(resolvedWithStatus.adjustments == expected);
}

TEST_CASE("resolveAdjustments reports malformed sidecar and falls back to defaultCrop", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("broken.arw");
    QFile f(XmpSidecar::pathFor(rawPath));
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write("<x:xmpmeta><rdf:RDF><rdf:Description crs:Exposure2012=\"2.0\"") > 0);
    f.close();

    const QRectF defaultCrop(0.2, 0.3, 0.4, 0.5);
    const auto resolved = XmpSidecar::resolveAdjustmentsWithStatus(rawPath, defaultCrop);

    GlobalAdjustment expected;
    expected.cropRect = defaultCrop;
    CHECK(resolved.status == SidecarLoadStatus::ParseError);
    REQUIRE(resolved.adjustments == expected);
}

TEST_CASE("resolveForImage drops metadata from malformed sidecar", "[xmp]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("broken.arw");
    QFile f(XmpSidecar::pathFor(rawPath));
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write("<x:xmpmeta><rdf:RDF><rdf:Description xmp:Rating=\"5\"") > 0);
    f.close();

    const QRectF defaultCrop(0.2, 0.3, 0.4, 0.5);
    const auto resolved = XmpSidecar::resolveForImage(rawPath, defaultCrop);

    GlobalAdjustment expected;
    expected.cropRect = defaultCrop;
    CHECK(resolved.status == SidecarLoadStatus::ParseError);
    CHECK(resolved.data.adjustments == expected);
    CHECK(resolved.data.metadata == UserMetadata{});
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

TEST_CASE("lens correction toggles round-trip through the arraw namespace", "[xmp][arraw][lens]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("lens.arw");

    GlobalAdjustment p;
    p.lensCorrectDistortion = true;
    p.lensCorrectVignetting = false;
    p.lensCorrectCA = true;

    REQUIRE(XmpSidecar::saveAdjustments(rawPath, p));
    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(rawPath);

    CHECK(loaded.lensCorrectDistortion);
    CHECK_FALSE(loaded.lensCorrectVignetting);
    CHECK(loaded.lensCorrectCA);
}

TEST_CASE("absent lens correction toggles load as off", "[xmp][arraw][lens]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("plain.arw");
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, GlobalAdjustment{}));
    const GlobalAdjustment loaded = XmpSidecar::loadAdjustments(rawPath);
    CHECK_FALSE(loaded.lensCorrectDistortion);
    CHECK_FALSE(loaded.lensCorrectVignetting);
    CHECK_FALSE(loaded.lensCorrectCA);
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

TEST_CASE("writer emits Effects controls in crs and Grain identity in arraw", "[xmp][crs]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("film.arw");
    GlobalAdjustment p;
    p.postCropVignetteAmount = -28.0f;
    p.postCropVignetteMidpoint = 61.0f;
    p.postCropVignetteFeather = 73.0f;
    p.grainAmount = 32.0f;
    p.grainSize = 44.0f;
    p.grainRoughness = 79.0f;
    p.grainSeed = 3735928559U;
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, p));

    QFile f(XmpSidecar::pathFor(rawPath));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString xml = QString::fromUtf8(f.readAll());

    CHECK(xml.contains(R"(crs:PostCropVignetteAmount="-28.0000")"));
    CHECK(xml.contains(R"(crs:PostCropVignetteMidpoint="61.0000")"));
    CHECK(xml.contains(R"(crs:PostCropVignetteFeather="73.0000")"));
    CHECK(xml.contains(R"(crs:GrainAmount="32.0000")"));
    CHECK(xml.contains(R"(crs:GrainSize="44.0000")"));
    CHECK(xml.contains(R"(crs:GrainFrequency="79.0000")"));
    CHECK(xml.contains(R"(arraw:GrainSeed="3735928559")"));
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
    CHECK_FALSE(xml.contains("dc:title"));
    CHECK_FALSE(xml.contains("dc:description"));
    CHECK_FALSE(xml.contains("dc:subject"));
    CHECK_FALSE(xml.contains("dc:creator"));
    CHECK_FALSE(xml.contains("dc:rights"));
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

TEST_CASE("saveMetadata preserves foreign XMP properties", "[xmp][compatibility]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("catalogued.nef");
    QFile sidecar(XmpSidecar::pathFor(rawPath));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    REQUIRE(
        sidecar.write(R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about=""
      xmlns:xmp="http://ns.adobe.com/xap/1.0/"
      xmlns:dc="http://purl.org/dc/elements/1.1/"
      dc:format="image/x-nikon-nef">
      <dc:coverage>Prague</dc:coverage>
    </rdf:Description>
  </rdf:RDF>
</x:xmpmeta>)xml")
        > 0);
    sidecar.close();

    REQUIRE(XmpSidecar::saveMetadata(rawPath, {4, ColourLabel::Blue}));

    REQUIRE(sidecar.open(QIODevice::ReadOnly));
    const QString saved = QString::fromUtf8(sidecar.readAll());
    CHECK(saved.contains(R"(dc:format="image/x-nikon-nef")"));
    QDomDocument document;
    REQUIRE(bool(document.setContent(saved, QDomDocument::ParseOption::UseNamespaceProcessing)));
    const QDomNodeList coverage
        = document.elementsByTagNameNS("http://purl.org/dc/elements/1.1/", "coverage");
    REQUIRE(coverage.size() == 1);
    CHECK(coverage.at(0).toElement().text() == "Prague");
    CHECK(XmpSidecar::loadMetadata(rawPath) == UserMetadata{4, ColourLabel::Blue});
}

TEST_CASE(
    "descriptive User Metadata replaces owned Dublin Core properties and preserves foreign "
    "metadata",
    "[xmp][metadata][dc]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("catalogued.nef");
    QFile sidecar(XmpSidecar::pathFor(rawPath));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    REQUIRE(
        sidecar.write(R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about=""
      xmlns:dc="http://purl.org/dc/elements/1.1/"
      xmlns:lr="http://ns.adobe.com/lightroom/1.0/">
      <dc:subject><rdf:Bag><rdf:li>Old</rdf:li></rdf:Bag></dc:subject>
      <dc:format>image/x-nikon-nef</dc:format>
      <lr:hierarchicalSubject>
        <rdf:Bag><rdf:li>Places|Prague</rdf:li></rdf:Bag>
      </lr:hierarchicalSubject>
    </rdf:Description>
  </rdf:RDF>
</x:xmpmeta>)xml")
        > 0);
    sidecar.close();

    UserMetadata loaded = XmpSidecar::loadMetadata(rawPath);
    CHECK(loaded.keywords == QStringList{"Old"});

    loaded.keywords = {"Family", "Travel"};
    loaded.title = "Summer edits";
    REQUIRE(XmpSidecar::saveMetadata(rawPath, loaded));

    const UserMetadata roundTripped = XmpSidecar::loadMetadata(rawPath);
    CHECK(roundTripped.title == "Summer edits");
    CHECK(roundTripped.keywords == QStringList{"Family", "Travel"});

    REQUIRE(sidecar.open(QIODevice::ReadOnly));
    QDomDocument document;
    REQUIRE(
        bool(document
                 .setContent(sidecar.readAll(), QDomDocument::ParseOption::UseNamespaceProcessing)));
    constexpr auto dcNamespace = "http://purl.org/dc/elements/1.1/";
    constexpr auto lrNamespace = "http://ns.adobe.com/lightroom/1.0/";
    const QDomNodeList subjects = document.elementsByTagNameNS(dcNamespace, "subject");
    REQUIRE(subjects.size() == 1);
    CHECK(subjects.at(0).toElement().text() == "FamilyTravel");
    const QDomNodeList formats = document.elementsByTagNameNS(dcNamespace, "format");
    REQUIRE(formats.size() == 1);
    CHECK(formats.at(0).toElement().text() == "image/x-nikon-nef");
    const QDomNodeList hierarchical
        = document.elementsByTagNameNS(lrNamespace, "hierarchicalSubject");
    REQUIRE(hierarchical.size() == 1);
    CHECK(hierarchical.at(0).toElement().text() == "Places|Prague");
}

TEST_CASE("embedded XMP packets parse descriptive User Metadata", "[xmp][metadata][dc]") {
    const QByteArray packet = R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/">
      <dc:title><rdf:Alt><rdf:li xml:lang="x-default">Embedded title</rdf:li></rdf:Alt></dc:title>
      <dc:description><rdf:Alt><rdf:li xml:lang="x-default">Embedded caption</rdf:li></rdf:Alt></dc:description>
      <dc:subject><rdf:Bag><rdf:li>One</rdf:li><rdf:li>Two</rdf:li></rdf:Bag></dc:subject>
      <dc:creator><rdf:Seq><rdf:li>Embedded creator</rdf:li></rdf:Seq></dc:creator>
      <dc:rights><rdf:Alt><rdf:li xml:lang="x-default">Embedded rights</rdf:li></rdf:Alt></dc:rights>
    </rdf:Description>
  </rdf:RDF>
</x:xmpmeta>)xml";

    const UserMetadata metadata = XmpSidecar::metadataFromPacket(packet);

    CHECK(metadata.title == "Embedded title");
    CHECK(metadata.caption == "Embedded caption");
    CHECK(metadata.keywords == QStringList{"One", "Two"});
    CHECK(metadata.creator == "Embedded creator");
    CHECK(metadata.copyright == "Embedded rights");
}

TEST_CASE(
    "Dublin Core property attributes parse as descriptive User Metadata", "[xmp][metadata][dc]") {
    const QByteArray packet = R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about=""
      xmlns:dc="http://purl.org/dc/elements/1.1/"
      dc:title="Attribute title"
      dc:description="Attribute caption"
      dc:creator="Attribute creator"
      dc:rights="Attribute rights" />
  </rdf:RDF>
</x:xmpmeta>)xml";

    const XmpPacketMetadata metadata = XmpSidecar::metadataPacketFromPacket(packet);

    CHECK(metadata.metadata.title == "Attribute title");
    CHECK(metadata.metadata.caption == "Attribute caption");
    CHECK(metadata.metadata.creator == "Attribute creator");
    CHECK(metadata.metadata.copyright == "Attribute rights");
    CHECK(metadata.presence.title);
    CHECK(metadata.presence.caption);
    CHECK(metadata.presence.creator);
    CHECK(metadata.presence.copyright);
}

TEST_CASE(
    "descriptive User Metadata writes the Lightroom-compatible RDF containers",
    "[xmp][metadata][dc]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("described.arw");
    UserMetadata metadata;
    metadata.title = "Title";
    metadata.caption = "Caption";
    metadata.keywords = {"One", "Two"};
    metadata.creator = "Creator";
    metadata.copyright = "Copyright";

    REQUIRE(XmpSidecar::saveMetadata(rawPath, metadata));

    QFile sidecar(XmpSidecar::pathFor(rawPath));
    REQUIRE(sidecar.open(QIODevice::ReadOnly));
    QDomDocument document;
    REQUIRE(
        bool(document
                 .setContent(sidecar.readAll(), QDomDocument::ParseOption::UseNamespaceProcessing)));
    constexpr auto dcNamespace = "http://purl.org/dc/elements/1.1/";
    constexpr auto rdfNamespace = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

    auto childContainer = [&](const char* dcName) {
        const QDomNodeList nodes = document.elementsByTagNameNS(dcNamespace, dcName);
        REQUIRE(nodes.size() == 1);
        return nodes.at(0).firstChildElement();
    };

    CHECK(childContainer("title").localName() == "Alt");
    CHECK(childContainer("description").localName() == "Alt");
    CHECK(childContainer("rights").localName() == "Alt");
    CHECK(childContainer("subject").localName() == "Bag");
    CHECK(childContainer("creator").localName() == "Seq");

    const QDomNodeList keywordItems
        = childContainer("subject").elementsByTagNameNS(rdfNamespace, "li");
    REQUIRE(keywordItems.size() == 2);
    CHECK(keywordItems.at(0).toElement().text() == "One");
    CHECK(keywordItems.at(1).toElement().text() == "Two");
}

TEST_CASE(
    "rating-only metadata saves preserve descriptive Dublin Core fields", "[xmp][metadata][dc]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("described.arw");
    UserMetadata metadata;
    metadata.caption = "Caption";
    UserMetadataPresence fields;
    fields.caption = true;
    REQUIRE(XmpSidecar::saveMetadata(rawPath, metadata, fields));

    UserMetadata ratingOnly;
    ratingOnly.rating = 5;
    REQUIRE(XmpSidecar::saveMetadata(rawPath, ratingOnly));

    const UserMetadata roundTripped = XmpSidecar::loadMetadata(rawPath);
    CHECK(roundTripped.rating == 5);
    CHECK(roundTripped.caption == "Caption");
}

TEST_CASE("authored empty Dublin Core fields suppress fallback reads", "[xmp][metadata][dc]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("described.arw");
    UserMetadata metadata;
    UserMetadataPresence fields;
    fields.caption = true;
    REQUIRE(XmpSidecar::saveMetadata(rawPath, metadata, fields));

    const SidecarData sidecar = XmpSidecar::load(rawPath);

    CHECK(sidecar.metadata.caption.isEmpty());
    CHECK(sidecar.metadataPresence.caption);
}

TEST_CASE("saveAdjustments preserves unowned XMP properties", "[xmp][compatibility]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("catalogued.nef");
    QFile sidecar(XmpSidecar::pathFor(rawPath));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    REQUIRE(
        sidecar.write(R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about=""
      xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
      xmlns:dc="http://purl.org/dc/elements/1.1/"
      crs:Exposure2012="-2.0"
      crs:AlreadyApplied="True">
      <dc:subject><rdf:Bag><rdf:li>Travel</rdf:li></rdf:Bag></dc:subject>
    </rdf:Description>
  </rdf:RDF>
</x:xmpmeta>)xml")
        > 0);
    sidecar.close();

    auto edits = sampleParams();
    edits.exposure = 1.5f;
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, edits));

    REQUIRE(sidecar.open(QIODevice::ReadOnly));
    QDomDocument document;
    REQUIRE(
        bool(document
                 .setContent(sidecar.readAll(), QDomDocument::ParseOption::UseNamespaceProcessing)));
    const QDomElement description
        = document.elementsByTagNameNS("http://www.w3.org/1999/02/22-rdf-syntax-ns#", "Description")
              .at(0)
              .toElement();
    CHECK(
        description.attributeNS("http://ns.adobe.com/camera-raw-settings/1.0/", "AlreadyApplied")
        == "True");
    CHECK(
        description.attributeNS("http://ns.adobe.com/camera-raw-settings/1.0/", "Exposure2012")
        == "1.5000");
    const QDomNodeList subjects
        = document.elementsByTagNameNS("http://purl.org/dc/elements/1.1/", "subject");
    REQUIRE(subjects.size() == 1);
    CHECK(subjects.at(0).toElement().text() == "Travel");
}

TEST_CASE("saveAdjustments replaces all arraw namespace content", "[xmp][compatibility]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("previous-version.nef");
    QFile sidecar(XmpSidecar::pathFor(rawPath));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    REQUIRE(
        sidecar.write(R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about=""
      xmlns:arraw="http://ns.arraw.app/1.0/"
      arraw:LegacyVersion="0">
      <arraw:Obsolete><rdf:Bag arraw:LegacyFlag="1"/></arraw:Obsolete>
    </rdf:Description>
  </rdf:RDF>
</x:xmpmeta>)xml")
        > 0);
    sidecar.close();

    GlobalAdjustment edits;
    LocalAdjustment local;
    local.mask = LinearMask{{0.1, 0.2}, {0.8, 0.9}};
    local.exposure = 0.75f;
    edits.localAdjustments.push_back(local);
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, edits));

    REQUIRE(sidecar.open(QIODevice::ReadOnly));
    QDomDocument document;
    REQUIRE(
        bool(document
                 .setContent(sidecar.readAll(), QDomDocument::ParseOption::UseNamespaceProcessing)));
    constexpr auto arrawNamespace = "http://ns.arraw.app/1.0/";
    CHECK(document.elementsByTagNameNS(arrawNamespace, "Obsolete").isEmpty());
    CHECK(document.elementsByTagNameNS(arrawNamespace, "LocalAdjustments").size() == 1);
    CHECK(document.elementsByTagNameNS(arrawNamespace, "Exposure").size() == 1);

    const QDomNodeList elements = document.elementsByTagName("*");
    for (int i = 0; i < elements.size(); ++i) {
        const QDomNamedNodeMap attributes = elements.at(i).attributes();
        for (int j = 0; j < attributes.size(); ++j)
            CHECK_FALSE(
                (attributes.item(j).namespaceURI() == arrawNamespace
                 && attributes.item(j).localName().startsWith("Legacy")));
    }
}

// ---------------------------------------------------------------------------
// Snapshots (docs/adr/0033): named, persisted A/B develop states living in the
// arraw: namespace as an arraw:Snapshots Seq of whole-develop-state resources.
// ---------------------------------------------------------------------------

TEST_CASE("snapshots round-trip through the sidecar with full state", "[xmp][snapshots]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("ab.dng");

    Snapshot warm;
    warm.name = "Warm look";
    warm.state = sampleParams();
    warm.state.temperature = 8000.0f;
    // Fields checkClose() doesn't cover — asserted explicitly below so the
    // snapshot encoding is guarded for the *whole* develop state, not a subset.
    warm.state.colorNoiseReduction = 40.0f;
    warm.state.colorNoiseReductionSmoothness = 65.0f;
    warm.state.lensCorrectDistortion = true;
    warm.state.lensCorrectCA = true;
    warm.state.orientation = orient::Orientation{1, true}; // turned + mirrored (EXIF 5)
    warm.state.cropConstrained = true;

    Snapshot cool;
    cool.name = "Cool look";
    cool.state.temperature = 4200.0f;
    cool.state.tint = 8.0f;

    REQUIRE(XmpSidecar::saveAdjustments(rawPath, GlobalAdjustment{}, {warm, cool}));
    const SidecarData loaded = XmpSidecar::load(rawPath);

    REQUIRE(loaded.snapshots.size() == 2);
    CHECK(loaded.snapshots[0].name == "Warm look");
    CHECK(loaded.snapshots[1].name == "Cool look");
    checkClose(loaded.snapshots[0].state, warm.state);
    checkCurveClose(loaded.snapshots[0].state.curveLuma, warm.state.curveLuma);
    checkCurveClose(loaded.snapshots[0].state.curveR, warm.state.curveR);
    CHECK_THAT(loaded.snapshots[0].state.colorNoiseReduction, WithinAbs(40.0, kScalarTol));
    CHECK_THAT(loaded.snapshots[0].state.colorNoiseReductionSmoothness, WithinAbs(65.0, kScalarTol));
    CHECK(loaded.snapshots[0].state.lensCorrectDistortion);
    CHECK_FALSE(loaded.snapshots[0].state.lensCorrectVignetting);
    CHECK(loaded.snapshots[0].state.lensCorrectCA);
    CHECK(loaded.snapshots[0].state.orientation == warm.state.orientation);
    CHECK(loaded.snapshots[0].state.cropConstrained);
    checkClose(loaded.snapshots[1].state, cool.state);
}

TEST_CASE("a snapshot carries its local adjustments and spots losslessly", "[xmp][snapshots]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("masked-snap.dng");

    Snapshot snap;
    snap.name = "Dodged sky";
    LocalAdjustment la;
    la.mask = RadialMask{{0.4, 0.6}, 0.3, 0.2, 30.0, 0.4, true};
    la.exposure = -0.75f;
    la.temperature = 25.0f; // relative
    snap.state.localAdjustments.push_back(la);
    snap.state.spots.push_back(Spot{{120.0, 80.0}, {200.0, 150.0}, 18.0, 0.3});

    REQUIRE(XmpSidecar::saveAdjustments(rawPath, GlobalAdjustment{}, {snap}));
    const SidecarData loaded = XmpSidecar::load(rawPath);

    // The top-level develop state stays clean — snapshot content must not leak up.
    CHECK(loaded.adjustments.localAdjustments.empty());
    CHECK(loaded.adjustments.spots.empty());

    REQUIRE(loaded.snapshots.size() == 1);
    const GlobalAdjustment& s = loaded.snapshots[0].state;
    REQUIRE(s.localAdjustments.size() == 1);
    REQUIRE(std::holds_alternative<RadialMask>(s.localAdjustments[0].mask));
    const RadialMask& m = std::get<RadialMask>(s.localAdjustments[0].mask);
    CHECK_THAT(m.center.x(), WithinAbs(0.4, kScalarTol));
    CHECK(m.invert);
    CHECK_THAT(s.localAdjustments[0].exposure, WithinAbs(-0.75, kScalarTol));
    CHECK_THAT(s.localAdjustments[0].temperature, WithinAbs(25.0, kScalarTol));
    REQUIRE(s.spots.size() == 1);
    CHECK_THAT(s.spots[0].destination.x(), WithinAbs(120.0, kScalarTol));
    CHECK_THAT(s.spots[0].source.y(), WithinAbs(150.0, kScalarTol));
    CHECK_THAT(s.spots[0].radius, WithinAbs(18.0, kScalarTol));
}

TEST_CASE("no snapshots writes no arraw:Snapshots element", "[xmp][snapshots]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("clean.dng");
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, sampleParams(), {}));

    QFile f(XmpSidecar::pathFor(rawPath));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString xml = QString::fromUtf8(f.readAll());
    CHECK_FALSE(xml.contains("Snapshots"));
}

TEST_CASE("the 2-arg saveAdjustments preserves existing snapshots", "[xmp][snapshots]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("keep-snaps.dng");

    Snapshot snap;
    snap.name = "Keeper";
    snap.state.exposure = 1.0f;
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, GlobalAdjustment{}, {snap}));

    // A plain develop save (no snapshot argument) must not drop them.
    GlobalAdjustment edits;
    edits.contrast = 20.0f;
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, edits));

    const SidecarData loaded = XmpSidecar::load(rawPath);
    CHECK_THAT(loaded.adjustments.contrast, WithinAbs(20.0, kScalarTol));
    REQUIRE(loaded.snapshots.size() == 1);
    CHECK(loaded.snapshots[0].name == "Keeper");
    CHECK_THAT(loaded.snapshots[0].state.exposure, WithinAbs(1.0, kScalarTol));
}

TEST_CASE("saving snapshots preserves foreign XMP properties", "[xmp][snapshots][compatibility]") {
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("catalogued.nef");
    QFile sidecar(XmpSidecar::pathFor(rawPath));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    REQUIRE(sidecar.write(R"xml(<?xml version="1.0"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about=""
      xmlns:dc="http://purl.org/dc/elements/1.1/">
      <dc:subject><rdf:Bag><rdf:li>Travel</rdf:li></rdf:Bag></dc:subject>
    </rdf:Description>
  </rdf:RDF>
</x:xmpmeta>)xml") > 0);
    sidecar.close();

    Snapshot snap;
    snap.name = "Warm look";
    snap.state.temperature = 8000.0f;
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, GlobalAdjustment{}, {snap}));

    REQUIRE(sidecar.open(QIODevice::ReadOnly));
    QDomDocument document;
    REQUIRE(bool(document.setContent(
        sidecar.readAll(), QDomDocument::ParseOption::UseNamespaceProcessing)));
    const QDomNodeList subjects
        = document.elementsByTagNameNS("http://purl.org/dc/elements/1.1/", "subject");
    REQUIRE(subjects.size() == 1);
    CHECK(subjects.at(0).toElement().text() == "Travel");
    CHECK(XmpSidecar::load(rawPath).snapshots.size() == 1);
}
