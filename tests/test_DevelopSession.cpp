#include "DevelopSession.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("DevelopSession starts empty", "[develop-session]") {
    DevelopSession session;

    CHECK(session.loadState() == DevelopSession::LoadState::Empty);
    CHECK(session.path().isEmpty());
    CHECK_FALSE(session.hasImage());
    CHECK_FALSE(session.developDirty());
    CHECK_FALSE(session.metadataDirty());
}

TEST_CASE("DevelopSession stores a loaded image as clean active state", "[develop-session]") {
    DevelopSession session;
    LoadResult result;
    result.preview = ImageBuffer{{0.1f, 0.2f, 0.3f}, 1, 1};
    result.fullRes = ImageBuffer{{0.4f, 0.5f, 0.6f}, 1, 1};
    result.metadata.rows.append(QPair<QString, QString>{"Make", "Camera"});
    result.defaultCrop = QRectF(0.1, 0.2, 0.7, 0.6);

    GlobalAdjustment params;
    params.exposure = 1.25f;

    session.setLoadedImage(
        "/photos/IMG_0001.dng", result, params, DevelopSession::SidecarState::Loaded);

    CHECK(session.loadState() == DevelopSession::LoadState::Loaded);
    CHECK(session.sidecarState() == DevelopSession::SidecarState::Loaded);
    CHECK(session.path() == "/photos/IMG_0001.dng");
    CHECK(session.hasImage());
    CHECK(session.preview().width == 1);
    CHECK(session.fullRes().data[0] == 0.4f);
    REQUIRE(session.metadata().rows.size() == 1);
    CHECK(session.metadata().rows[0].first == "Make");
    CHECK(session.metadata().rows[0].second == "Camera");
    CHECK(session.defaultCrop() == QRectF(0.1, 0.2, 0.7, 0.6));
    CHECK(session.params().exposure == 1.25f);
    CHECK_FALSE(session.developDirty());
    CHECK_FALSE(session.metadataDirty());
}

TEST_CASE("DevelopSession tracks dirty develop edits against the saved baseline",
          "[develop-session]") {
    DevelopSession session;
    LoadResult result;
    result.preview = ImageBuffer{{0.1f, 0.2f, 0.3f}, 1, 1};

    GlobalAdjustment saved;
    saved.exposure = 0.5f;
    session.setLoadedImage(
        "/photos/IMG_0001.dng", result, saved, DevelopSession::SidecarState::Loaded);

    GlobalAdjustment edited = saved;
    edited.exposure = 1.0f;
    session.setParams(edited);

    CHECK(session.params().exposure == 1.0f);
    CHECK(session.developDirty());
    CHECK_FALSE(session.metadataDirty());

    session.markDevelopSaved();

    CHECK_FALSE(session.developDirty());
    CHECK_FALSE(session.metadataDirty());
}

TEST_CASE("DevelopSession tracks local adjustments as canonical develop edits",
          "[develop-session]") {
    DevelopSession session;
    LoadResult result;
    result.preview = ImageBuffer{{0.1f, 0.2f, 0.3f}, 1, 1};
    session.setLoadedImage(
        "/photos/IMG_0001.dng",
        result,
        GlobalAdjustment{},
        DevelopSession::SidecarState::Loaded);

    LocalAdjustment adjustment;
    adjustment.exposure = 0.75f;
    adjustment.mask = LinearMask{{0.0, 0.0}, {1.0, 1.0}};
    session.setLocalAdjustments({adjustment});

    REQUIRE(session.params().localAdjustments.size() == 1);
    CHECK(session.params().localAdjustments[0].exposure == 0.75f);
    CHECK(session.developDirty());
    CHECK_FALSE(session.metadataDirty());
}

TEST_CASE("DevelopSession tracks user metadata dirty state separately", "[develop-session]") {
    DevelopSession session;
    LoadResult result;
    result.preview = ImageBuffer{{0.1f, 0.2f, 0.3f}, 1, 1};

    UserMetadata savedMetadata{3, ColourLabel::Blue};
    session.setLoadedImage(
        "/photos/IMG_0001.dng",
        result,
        GlobalAdjustment{},
        DevelopSession::SidecarState::Loaded,
        savedMetadata);

    UserMetadata editedMetadata = savedMetadata;
    editedMetadata.rating = 4;
    session.setUserMetadata(editedMetadata);

    const UserMetadata expectedMetadata{4, ColourLabel::Blue};
    CHECK(session.userMetadata() == expectedMetadata);
    CHECK_FALSE(session.developDirty());
    CHECK(session.metadataDirty());

    session.markMetadataSaved();

    CHECK_FALSE(session.developDirty());
    CHECK_FALSE(session.metadataDirty());
}

TEST_CASE("DevelopSession records sidecar write failures without clearing dirty state",
          "[develop-session]") {
    DevelopSession session;
    LoadResult result;
    result.preview = ImageBuffer{{0.1f, 0.2f, 0.3f}, 1, 1};
    session.setLoadedImage(
        "/photos/IMG_0001.dng",
        result,
        GlobalAdjustment{},
        DevelopSession::SidecarState::Loaded,
        UserMetadata{});

    GlobalAdjustment edited = session.params();
    edited.exposure = 0.5f;
    session.setParams(edited);
    session.markDevelopSaveFailed();

    CHECK(session.sidecarState() == DevelopSession::SidecarState::WriteError);
    CHECK(session.developDirty());

    UserMetadata metadata;
    metadata.rating = 5;
    session.setUserMetadata(metadata);
    session.markMetadataSaveFailed();

    CHECK(session.sidecarState() == DevelopSession::SidecarState::WriteError);
    CHECK(session.metadataDirty());
}

TEST_CASE("DevelopSession successful saves mark the sidecar loaded", "[develop-session]") {
    DevelopSession session;
    LoadResult result;
    result.preview = ImageBuffer{{0.1f, 0.2f, 0.3f}, 1, 1};
    session.setLoadedImage(
        "/photos/IMG_0001.dng",
        result,
        GlobalAdjustment{},
        DevelopSession::SidecarState::Missing,
        UserMetadata{});

    GlobalAdjustment edited = session.params();
    edited.exposure = 0.5f;
    session.setParams(edited);
    session.markDevelopSaved();

    CHECK(session.sidecarState() == DevelopSession::SidecarState::Loaded);
    CHECK_FALSE(session.developDirty());

    UserMetadata metadata;
    metadata.rating = 4;
    session.setUserMetadata(metadata);
    session.markMetadataSaved();

    CHECK(session.sidecarState() == DevelopSession::SidecarState::Loaded);
    CHECK_FALSE(session.metadataDirty());
}

TEST_CASE("DevelopSession exposes spot-applied buffers by display and export intent",
          "[develop-session]") {
    DevelopSession session;
    LoadResult result;
    result.preview = ImageBuffer{{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, 2, 1};
    result.fullRes = result.preview;
    session.setLoadedImage(
        "/photos/IMG_0001.dng",
        result,
        GlobalAdjustment{},
        DevelopSession::SidecarState::Loaded);

    Spot spot;
    spot.destination = {0.0, 0.0};
    spot.source = {1.0, 0.0};
    spot.radius = 1.0;
    spot.feather = 0.0;
    session.setSpots({spot});

    REQUIRE(session.params().spots.size() == 1);
    CHECK(session.developDirty());
    CHECK(session.previewForDisplay().data[0] == 1.0f);
    CHECK(session.fullResForExport().data[0] == 1.0f);
}

TEST_CASE("DevelopSession scales full-res spot coordinates for preview display",
          "[develop-session]") {
    DevelopSession session;
    LoadResult result;
    result.preview = ImageBuffer{
        {0.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 0.0f,
         1.0f, 0.0f, 0.0f,
         0.0f, 1.0f, 0.0f},
        4,
        1};
    result.fullRes = ImageBuffer{
        {0.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 0.0f,
         1.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 0.0f,
         0.0f, 1.0f, 0.0f},
        8,
        1};
    session.setLoadedImage(
        "/photos/IMG_0001.dng",
        result,
        GlobalAdjustment{},
        DevelopSession::SidecarState::Loaded);

    Spot spot;
    spot.destination = {0.0, 0.0};
    spot.source = {4.0, 0.0};
    spot.radius = 1.0;
    spot.feather = 0.0;
    session.setSpots({spot});

    CHECK(session.previewForDisplay().data[0] == 1.0f);
    CHECK(session.previewForDisplay().data[1] == 0.0f);
    CHECK(session.fullResForExport().data[0] == 1.0f);
}

TEST_CASE("DevelopSession records the image being loaded", "[develop-session]") {
    DevelopSession session;

    session.beginLoading("/photos/IMG_0001.dng");

    CHECK(session.loadState() == DevelopSession::LoadState::Loading);
    CHECK(session.path() == "/photos/IMG_0001.dng");
    CHECK_FALSE(session.hasImage());
    CHECK_FALSE(session.developDirty());
    CHECK_FALSE(session.metadataDirty());
}

TEST_CASE("DevelopSession applies lens correction only when toggled", "[develop-session][lens]") {
    DevelopSession session;
    LoadResult result;
    const auto flat = [](int w, int h, float v) {
        ImageBuffer b;
        b.width = w;
        b.height = h;
        b.data.assign(static_cast<size_t>(w * h * 3), v);
        return b;
    };
    result.fullRes = flat(8, 8, 0.25f);
    result.preview = flat(8, 8, 0.25f);
    // A vignette gain profile: identity at the centre, brightening toward the corners.
    result.lensModel.vignette = RadialCurve::fromFn([](float r) { return 1.0f + r; });
    result.lensModel.hasVignetting = true;

    GlobalAdjustment params; // all lens toggles default off
    session.setLoadedImage("/p.arw", result, params, DevelopSession::SidecarState::Loaded);

    // Toggle off → display is the clean buffer untouched.
    CHECK(session.previewForDisplay().data == result.preview.data);

    // Toggle on → corner brightened, centre ~unchanged; display is the corrected buffer.
    params.lensCorrectVignetting = true;
    session.setParams(params);
    const ImageBuffer& on = session.previewForDisplay();
    CHECK(on.data != result.preview.data);
    CHECK(on.data[1] > 0.30f);                                 // corner (0,0) green brightened
    CHECK(on.data[static_cast<size_t>((4 * 8 + 4) * 3 + 1)] < 0.30f); // near centre ~unchanged
    CHECK(session.fullResForExport().data[1] > 0.30f);        // full-res corrected too

    // Toggle back off → clean again.
    params.lensCorrectVignetting = false;
    session.setParams(params);
    CHECK(session.previewForDisplay().data == result.preview.data);
}
