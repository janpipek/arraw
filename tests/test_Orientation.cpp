#include "Orientation.h"

#include <catch2/catch_test_macros.hpp>

#include <QPointF>

#include <cmath>
#include <set>
#include <utility>

using orient::Orientation;

TEST_CASE("EXIF 1 (normal) is the identity orientation") {
    REQUIRE(orient::fromExif(1) == Orientation{0, false});
}

TEST_CASE("EXIF 6 is a 90° clockwise turn, no mirror") {
    REQUIRE(orient::fromExif(6) == Orientation{1, false});
}

TEST_CASE("EXIF 2 is a horizontal mirror, no turn") {
    REQUIRE(orient::fromExif(2) == Orientation{0, true});
}

TEST_CASE("every EXIF value 1..8 round-trips through fromExif/toExif") {
    for (int exif = 1; exif <= 8; ++exif) {
        INFO("exif = " << exif);
        REQUIRE(orient::toExif(orient::fromExif(exif)) == exif);
    }
}

TEST_CASE("the eight EXIF values map to eight distinct orientations") {
    std::set<std::pair<int, bool>> seen;
    for (int exif = 1; exif <= 8; ++exif) {
        const Orientation o = orient::fromExif(exif);
        seen.insert({o.quarterTurnsCW, o.mirrored});
    }
    REQUIRE(seen.size() == 8);
}

static bool uvEq(QPointF a, QPointF b) {
    return std::abs(a.x() - b.x()) < 1e-6 && std::abs(a.y() - b.y()) < 1e-6;
}

TEST_CASE("identity orientation leaves a UV point untouched") {
    REQUIRE(uvEq(orient::orientedToBuffer({0.3, 0.7}, Orientation{0, false}), {0.3, 0.7}));
}

TEST_CASE("90° CW: display corners sample the native corners turned back") {
    const Orientation cw{1, false};
    // Turning the native buffer 90° CW, its bottom-left lands at the display's
    // top-left — so the display's top-left samples the native bottom-left.
    REQUIRE(uvEq(orient::orientedToBuffer({0.0, 0.0}, cw), {0.0, 1.0})); // TL ← native BL
    REQUIRE(uvEq(orient::orientedToBuffer({1.0, 0.0}, cw), {0.0, 0.0})); // TR ← native TL
}

TEST_CASE("only odd quarter-turns swap width and height") {
    REQUIRE_FALSE(orient::swapsAspect(Orientation{0, false}));
    REQUIRE(orient::swapsAspect(Orientation{1, false}));
    REQUIRE_FALSE(orient::swapsAspect(Orientation{2, true}));
    REQUIRE(orient::swapsAspect(Orientation{3, false}));
}

TEST_CASE("every orientation maps the four corners onto the four distinct corners") {
    const QPointF corners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int exif = 1; exif <= 8; ++exif) {
        const Orientation o = orient::fromExif(exif);
        std::set<std::pair<int, int>> mapped;
        for (const QPointF& c : corners) {
            const QPointF b = orient::orientedToBuffer(c, o);
            mapped.insert({int(std::lround(b.x())), int(std::lround(b.y()))});
        }
        INFO("exif = " << exif);
        REQUIRE(mapped.size() == 4); // a bijection on the unit square's corners
    }
}
