#include "ui/FilmStripLayout.h"
#include <catch2/catch_test_macros.hpp>

#include <QSize>

TEST_CASE("cellWidth returns a square cell side for any loaded thumbnail", "[filmstrip]") {
    REQUIRE(filmstrip::cellWidth(100, QSize(300, 200)) == 100);
    REQUIRE(filmstrip::cellWidth(100, QSize(200, 300)) == 100);
    REQUIRE(filmstrip::cellWidth(100, QSize(100, 100)) == 100);
}

TEST_CASE("cellWidth guards non-positive strip heights", "[filmstrip]") {
    REQUIRE(filmstrip::cellWidth(100, QSize(0, 0)) == 100);
    REQUIRE(filmstrip::cellWidth(100, QSize(300, 0)) == 100);
    REQUIRE(filmstrip::cellWidth(0, QSize(300, 200)) == 0);
}
