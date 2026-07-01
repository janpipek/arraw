#include "MainWindowZoom.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("matchingZoomPresetIndex finds exact preset matches", "[zoom]") {
    CHECK(matchingZoomPresetIndex(0.25f) == 0);
    CHECK(matchingZoomPresetIndex(0.5f) == 1);
    CHECK(matchingZoomPresetIndex(1.0f) == 2);
    CHECK(matchingZoomPresetIndex(2.0f) == 3);
    CHECK(matchingZoomPresetIndex(4.0f) == 4);
}

TEST_CASE("matchingZoomPresetIndex tolerates float round-trip error", "[zoom]") {
    CHECK(matchingZoomPresetIndex(1.0f + 0.001f) == 2);
    CHECK(matchingZoomPresetIndex(1.0f - 0.001f) == 2);
}

TEST_CASE("matchingZoomPresetIndex rejects values outside tolerance", "[zoom]") {
    CHECK(matchingZoomPresetIndex(1.006f) == -1);   // just past the epsilon
    CHECK(matchingZoomPresetIndex(0.75f) == -1);    // between 50% and 100%
    CHECK(matchingZoomPresetIndex(0.125f) == -1);   // below the smallest preset
    CHECK(matchingZoomPresetIndex(0.0f) == -1);     // no image loaded (viewport::pixelZoom() == 0)
}
