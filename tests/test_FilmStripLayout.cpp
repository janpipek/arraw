#include <catch2/catch_test_macros.hpp>
#include "FilmStripLayout.h"

#include <QSize>

TEST_CASE("cellWidth fits the thumbnail to the strip height by aspect ratio",
          "[filmstrip]") {
    // 3:2 landscape at 100px tall -> 150px wide
    REQUIRE(filmstrip::cellWidth(100, QSize(300, 200)) == 150);
    // 2:3 portrait at 100px tall -> 67px wide (narrower than tall)
    REQUIRE(filmstrip::cellWidth(100, QSize(200, 300)) == 67);
}

TEST_CASE("cellWidth falls back to a square cell for degenerate sizes",
          "[filmstrip]") {
    REQUIRE(filmstrip::cellWidth(100, QSize(0, 0)) == 100);
    REQUIRE(filmstrip::cellWidth(100, QSize(300, 0)) == 100);
    REQUIRE(filmstrip::cellWidth(0, QSize(300, 200)) == 0);
}
