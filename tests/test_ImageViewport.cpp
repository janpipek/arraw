#include "ui/ImageViewport.h"

#include "TestApp.h"

#include <catch2/catch_test_macros.hpp>

namespace {

ImageBuffer solidBuffer(int w, int h) {
    ImageBuffer b;
    b.width = w;
    b.height = h;
    b.data.assign(size_t(w) * size_t(h) * 3, 0.5f);
    return b;
}

} // namespace

TEST_CASE("setImage refits by default but preserveView holds the zoom", "[viewport]") {
    testApp();
    ImageViewport vp;
    vp.resize(200, 150);
    vp.setImage(solidBuffer(64, 48), {}, false);
    const float fit = vp.zoomFactor();

    vp.setZoom(4.0f);
    REQUIRE(vp.zoomFactor() == 4.0f);

    // In-place swap of the same image (demosaic re-decode, lens toggle): the
    // user's zoom must hold.
    vp.setImage(solidBuffer(64, 48), {}, false, /*preserveView=*/true);
    CHECK(vp.zoomFactor() == 4.0f);

    // Default swap (fresh image): refit.
    vp.setImage(solidBuffer(64, 48), {}, false);
    CHECK(vp.zoomFactor() == fit);
}

TEST_CASE("preserved zoom past the full-res threshold re-requests full-res", "[viewport]") {
    testApp();
    ImageViewport vp;
    vp.resize(200, 150);
    vp.setImage(solidBuffer(64, 48), {}, false);

    int requests = 0;
    QObject::connect(&vp, &ImageViewport::fullResNeeded, &vp, [&requests] { ++requests; });

    vp.setZoom(4.0f); // crosses the threshold → one request
    REQUIRE(requests == 1);

    // setImage clears the full-res slot; a preserved zoom sees no threshold
    // crossing in setZoom, so the swap itself must re-request full-res.
    vp.setImage(solidBuffer(64, 48), {}, false, /*preserveView=*/true);
    CHECK(requests == 2);
}

TEST_CASE("preserved zoom below the full-res threshold requests nothing", "[viewport]") {
    testApp();
    ImageViewport vp;
    vp.resize(200, 150);
    vp.setImage(solidBuffer(64, 48), {}, false);

    int requests = 0;
    QObject::connect(&vp, &ImageViewport::fullResNeeded, &vp, [&requests] { ++requests; });

    vp.setZoom(1.2f);
    vp.setImage(solidBuffer(64, 48), {}, false, /*preserveView=*/true);
    CHECK(vp.zoomFactor() == 1.2f);
    CHECK(requests == 0);
}
