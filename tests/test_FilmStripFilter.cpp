#include "develop/UserMetadata.h"
#include "ui/FilmStripFilter.h"
#include <catch2/catch_test_macros.hpp>

// Pure value-type tests: no QApplication, model, or view (ADR 0037).

namespace {
UserMetadata shot(int rating, ColourLabel label = ColourLabel::None) {
    UserMetadata m;
    m.rating = rating;
    m.label = label;
    return m;
}
} // namespace

TEST_CASE("default filter is inactive and matches everything", "[filmstrip][filter]") {
    FilmStripFilter f;
    REQUIRE_FALSE(f.isActive());
    REQUIRE(f.matches(shot(-1)));
    REQUIRE(f.matches(shot(0)));
    REQUIRE(f.matches(shot(5, ColourLabel::Blue)));
}

TEST_CASE("star threshold matches N and above, hides below and rejects", "[filmstrip][filter]") {
    FilmStripFilter f;
    f.minRating = 3;
    REQUIRE(f.isActive());

    REQUIRE(f.matches(shot(3)));
    REQUIRE(f.matches(shot(4)));
    REQUIRE(f.matches(shot(5)));

    REQUIRE_FALSE(f.matches(shot(2)));
    REQUIRE_FALSE(f.matches(shot(1)));
    REQUIRE_FALSE(f.matches(shot(0)));  // unrated
    REQUIRE_FALSE(f.matches(shot(-1))); // reject
}

TEST_CASE("rejects-only matches only rejects", "[filmstrip][filter]") {
    FilmStripFilter f;
    f.rejectsOnly = true;
    REQUIRE(f.isActive());

    REQUIRE(f.matches(shot(-1)));
    REQUIRE_FALSE(f.matches(shot(0)));
    REQUIRE_FALSE(f.matches(shot(3)));
    REQUIRE_FALSE(f.matches(shot(5)));
}

TEST_CASE("colour set matches any chosen colour (OR)", "[filmstrip][filter]") {
    FilmStripFilter f;
    f.colours = {ColourLabel::Red, ColourLabel::Green};
    REQUIRE(f.isActive());

    REQUIRE(f.matches(shot(0, ColourLabel::Red)));
    REQUIRE(f.matches(shot(0, ColourLabel::Green)));

    REQUIRE_FALSE(f.matches(shot(0, ColourLabel::Blue)));
    REQUIRE_FALSE(f.matches(shot(0, ColourLabel::None)));
}

TEST_CASE("empty colour set imposes no colour constraint", "[filmstrip][filter]") {
    FilmStripFilter f;
    f.minRating = 1; // active via rating, colours empty
    REQUIRE(f.matches(shot(2, ColourLabel::None)));
    REQUIRE(f.matches(shot(2, ColourLabel::Purple)));
}

TEST_CASE("star and colour combine with AND", "[filmstrip][filter]") {
    FilmStripFilter f;
    f.minRating = 3;
    f.colours = {ColourLabel::Red, ColourLabel::Green};

    REQUIRE(f.matches(shot(4, ColourLabel::Green))); // meets both
    REQUIRE(f.matches(shot(3, ColourLabel::Red)));

    REQUIRE_FALSE(f.matches(shot(4, ColourLabel::Blue))); // colour fails
    REQUIRE_FALSE(f.matches(shot(2, ColourLabel::Red)));  // rating fails
    REQUIRE_FALSE(f.matches(shot(2, ColourLabel::Blue))); // both fail
}

TEST_CASE("equality compares all fields", "[filmstrip][filter]") {
    FilmStripFilter a;
    FilmStripFilter b;
    REQUIRE(a == b);

    b.minRating = 2;
    REQUIRE_FALSE(a == b);

    a.minRating = 2;
    a.colours = {ColourLabel::Blue};
    b.colours = {ColourLabel::Blue};
    REQUIRE(a == b);
}
