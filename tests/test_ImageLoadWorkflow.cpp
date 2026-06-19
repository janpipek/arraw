#include "ImageLoadWorkflow.h"
#include "XmpSidecar.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
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
