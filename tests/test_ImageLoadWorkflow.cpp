#include "ImageLoadWorkflow.h"
#include "core/ImageMetadata.h"
#include "develop/DemosaicAlgorithm.h"
#include "develop/GlobalAdjustment.h"
#include "develop/UserMetadata.h"
#include "io/XmpSidecar.h"
#include "pipeline/LoadResult.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QImage>
#include <QTemporaryDir>

TEST_CASE("decodeCacheKey changes when file metadata changes", "[loadworkflow]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("image.jpg");

    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("one");
    file.close();
    const QString first = decodeCacheKey(path);

    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Append));
    file.write("two");
    file.close();

    CHECK(decodeCacheKey(path) != first);
}

TEST_CASE("decodeCacheKey distinguishes demosaic algorithms for the same file", "[loadworkflow]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("image.jpg");

    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("pixels");
    file.close();

    const QString ahd = decodeCacheKey(path, DemosaicAlgorithm::AHD);
    const QString vng = decodeCacheKey(path, DemosaicAlgorithm::VNG);

    // Each algorithm's decode caches independently (ADR 0036).
    CHECK(ahd != vng);
    // ...but the key is stable for the same (path, algo) so A/B switching hits.
    CHECK(decodeCacheKey(path, DemosaicAlgorithm::VNG) == vng);
    // The token is what disambiguates.
    CHECK(ahd.endsWith("|" + demosaicToken(DemosaicAlgorithm::AHD)));
    CHECK(vng.endsWith("|" + demosaicToken(DemosaicAlgorithm::VNG)));
    // Default argument == kDefaultDemosaic, so the no-algo overload matches AHD.
    CHECK(decodeCacheKey(path) == ahd);
}

TEST_CASE("resolvePendingPreviewParams uses a full-frame placeholder crop", "[loadworkflow]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString rawPath = dir.filePath("IMG_0001.CR3");

    const GlobalAdjustment params = resolvePendingPreviewParams(rawPath);
    CHECK(params.cropRect == QRectF(0.0, 0.0, 1.0, 1.0));
}

TEST_CASE("resolveLoadedImage maps loaded sidecars to session state", "[loadworkflow]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString rawPath = dir.filePath("IMG_0001.CR3");

    GlobalAdjustment saved;
    saved.exposure = 1.5f;
    UserMetadata metadata;
    metadata.rating = 4;
    REQUIRE(XmpSidecar::saveAdjustments(rawPath, saved));
    REQUIRE(XmpSidecar::saveMetadata(rawPath, metadata));

    LoadResult result;
    result.defaultCrop = QRectF(0.25, 0.25, 0.5, 0.5);
    const ResolvedLoadedImage resolved = resolveLoadedImage(rawPath, result);

    CHECK(resolved.sidecarState == DevelopSession::SidecarState::Loaded);
    CHECK(resolved.adjustments.exposure == 1.5f);
    CHECK(resolved.metadata.rating == 4);
}

// User Metadata read precedence is pure merge policy — EXIF prefill, overlaid by
// the embedded XMP packet, overlaid by the sidecar. It is exercised directly on
// the resolveUserMetadata seam with in-memory inputs (no disk, no XMP parsing),
// which is the merge's actual contract; resolveLoadedImage just feeds it.
TEST_CASE("resolveUserMetadata applies User Metadata read precedence", "[loadworkflow]") {
    ImageMetadata exif;
    exif.rows.append(qMakePair(QString("Description"), QString("EXIF caption")));
    exif.rows.append(qMakePair(QString("Artist"), QString("EXIF creator")));

    XmpPacketMetadata embedded;
    embedded.metadata.caption = "Embedded caption";
    embedded.metadata.creator = "Embedded creator";
    embedded.presence.caption = true;
    embedded.presence.creator = true;

    // No sidecar: the embedded packet overlays the EXIF prefill.
    SidecarData sidecar;
    UserMetadata resolved = resolveUserMetadata(sidecar, exif, embedded).metadata;
    CHECK(resolved.caption == "Embedded caption");
    CHECK(resolved.creator == "Embedded creator");

    // Sidecar caption present: it overlays the embedded value; creator, absent
    // from the sidecar, stays the embedded one.
    sidecar.metadata.caption = "Sidecar caption";
    sidecar.metadataPresence.caption = true;
    resolved = resolveUserMetadata(sidecar, exif, embedded).metadata;
    CHECK(resolved.caption == "Sidecar caption");
    CHECK(resolved.creator == "Embedded creator");
}

TEST_CASE("resolveUserMetadata honours sidecar-authored empty User Metadata", "[loadworkflow]") {
    ImageMetadata exif;
    exif.rows.append(qMakePair(QString("Description"), QString("EXIF caption")));

    XmpPacketMetadata embedded;
    embedded.metadata.caption = "Embedded caption";
    embedded.presence.caption = true;

    // The sidecar authored caption as an explicit empty value: present but blank.
    SidecarData sidecar;
    sidecar.metadataPresence.caption = true;

    const ResolvedUserMetadata resolved = resolveUserMetadata(sidecar, exif, embedded);
    CHECK(resolved.metadata.caption.isEmpty());
    CHECK(resolved.presence.caption);
}

TEST_CASE(
    "resolveLoadedImage reports malformed sidecars and applies default crop", "[loadworkflow]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString rawPath = dir.filePath("IMG_0001.CR3");

    QFile sidecar(XmpSidecar::pathFor(rawPath));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    sidecar.write("<not xml");
    sidecar.close();

    LoadResult result;
    result.defaultCrop = QRectF(0.25, 0.25, 0.5, 0.5);
    const ResolvedLoadedImage resolved = resolveLoadedImage(rawPath, result);

    CHECK(resolved.sidecarState == DevelopSession::SidecarState::ParseError);
    CHECK(resolved.adjustments.cropRect == result.defaultCrop);
}

TEST_CASE("leaving an image only needs confirmation when loaded state is dirty", "[loadworkflow]") {
    DevelopSession session;

    CHECK_FALSE(shouldConfirmLeavingImage(session));

    LoadResult result;
    result.preview = ImageBuffer{{0.1f, 0.2f, 0.3f}, 1, 1};
    session.setLoadedImage(
        "/photos/IMG_0001.CR3",
        result,
        GlobalAdjustment{},
        DevelopSession::SidecarState::Loaded,
        UserMetadata{});

    CHECK_FALSE(shouldConfirmLeavingImage(session));

    GlobalAdjustment edited = session.params();
    edited.exposure = 1.0f;
    session.setParams(edited);

    CHECK(shouldConfirmLeavingImage(session));

    session.markDevelopSaved();
    UserMetadata metadata;
    metadata.rating = 4;
    session.setUserMetadata(metadata);

    CHECK(shouldConfirmLeavingImage(session));

    session.beginLoading("/photos/IMG_0002.CR3");

    CHECK_FALSE(shouldConfirmLeavingImage(session));
}

TEST_CASE(
    "decodeImage loads standard image formats without embedded preview callback", "[loadworkflow]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("image.png");

    QImage image(2, 2, QImage::Format_RGB888);
    image.fill(Qt::white);
    REQUIRE(image.save(path));

    bool previewCalled = false;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    const LoadResult result
        = decodeImage(path, [&previewCalled](ImageBuffer) { previewCalled = true; }, cancel);

    CHECK(result.error.isEmpty());
    CHECK(result.fullRes.valid());
    CHECK_FALSE(previewCalled);
}
