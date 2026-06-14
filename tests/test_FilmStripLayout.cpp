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

TEST_CASE("centerScrollOffset centers the current item", "[filmstrip]") {
    // 10 cells of pitch 100 (contentWidth 1000), viewport 300.
    // item 5 center = 5*100 + 50 = 550; desired offset = 550 - 150 = 400.
    REQUIRE(filmstrip::centerScrollOffset(5, 100, 300, 1000) == 400);
}

TEST_CASE("centerScrollOffset clamps at both ends", "[filmstrip]") {
    // First item can't scroll negative.
    REQUIRE(filmstrip::centerScrollOffset(0, 100, 300, 1000) == 0);
    // Last item clamps to max = contentWidth - viewportWidth = 700.
    REQUIRE(filmstrip::centerScrollOffset(9, 100, 300, 1000) == 700);
    // Content narrower than the viewport never scrolls.
    REQUIRE(filmstrip::centerScrollOffset(0, 100, 300, 200) == 0);
}
