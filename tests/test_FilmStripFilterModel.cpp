#include "TestApp.h"
#include "develop/UserMetadata.h"
#include "ui/FilmStripFilter.h"
#include "ui/FilmStripFilterModel.h"
#include "ui/FilmStripModel.h"
#include <catch2/catch_test_macros.hpp>

#include <QStringList>

// The proxy needs an application object for the item-model machinery, like the
// FilmStripModel tests (one shared QApplication across the binary).
namespace {
void ensureApp() {
    testApp();
}

UserMetadata mark(int rating, ColourLabel label = ColourLabel::None) {
    UserMetadata m;
    m.rating = rating;
    m.label = label;
    return m;
}

QStringList visiblePaths(const FilmStripFilterModel& proxy) {
    QStringList paths;
    for (int row = 0; row < proxy.rowCount(); ++row)
        paths << proxy.index(row, 0).data(FilmStripModel::PathRole).toString();
    return paths;
}
} // namespace

TEST_CASE("inactive filter passes every source row through", "[filmstrip][filter]") {
    ensureApp();
    FilmStripModel source;
    source.setFiles({"/p/a.dng", "/p/b.dng", "/p/c.dng"});

    FilmStripFilterModel proxy;
    proxy.setSourceModel(&source);

    REQUIRE(proxy.rowCount() == 3);
    REQUIRE(visiblePaths(proxy) == QStringList{"/p/a.dng", "/p/b.dng", "/p/c.dng"});
}

TEST_CASE("star threshold hides rows below N", "[filmstrip][filter]") {
    ensureApp();
    FilmStripModel source;
    source.setFiles({"/p/a.dng", "/p/b.dng", "/p/c.dng", "/p/d.dng"});
    source.setMarks("/p/a.dng", mark(1));
    source.setMarks("/p/b.dng", mark(3));
    source.setMarks("/p/c.dng", mark(5));
    // d.dng left unrated

    FilmStripFilterModel proxy;
    proxy.setSourceModel(&source);

    FilmStripFilter f;
    f.minRating = 3;
    proxy.setFilter(f);

    REQUIRE(visiblePaths(proxy) == QStringList{"/p/b.dng", "/p/c.dng"});
}

TEST_CASE("colour OR set hides rows without a chosen colour", "[filmstrip][filter]") {
    ensureApp();
    FilmStripModel source;
    source.setFiles({"/p/a.dng", "/p/b.dng", "/p/c.dng"});
    source.setMarks("/p/a.dng", mark(0, ColourLabel::Red));
    source.setMarks("/p/b.dng", mark(0, ColourLabel::Green));
    source.setMarks("/p/c.dng", mark(0, ColourLabel::Blue));

    FilmStripFilterModel proxy;
    proxy.setSourceModel(&source);

    FilmStripFilter f;
    f.colours = {ColourLabel::Red, ColourLabel::Green};
    proxy.setFilter(f);

    REQUIRE(visiblePaths(proxy) == QStringList{"/p/a.dng", "/p/b.dng"});
}

TEST_CASE("rejects-only shows only rejects", "[filmstrip][filter]") {
    ensureApp();
    FilmStripModel source;
    source.setFiles({"/p/a.dng", "/p/b.dng", "/p/c.dng"});
    source.setMarks("/p/a.dng", mark(-1));
    source.setMarks("/p/b.dng", mark(4));

    FilmStripFilterModel proxy;
    proxy.setSourceModel(&source);

    FilmStripFilter f;
    f.rejectsOnly = true;
    proxy.setFilter(f);

    REQUIRE(visiblePaths(proxy) == QStringList{"/p/a.dng"});
}

TEST_CASE("a mark write on the source re-filters dynamically", "[filmstrip][filter]") {
    ensureApp();
    FilmStripModel source;
    source.setFiles({"/p/a.dng", "/p/b.dng"});
    source.setMarks("/p/a.dng", mark(1));
    source.setMarks("/p/b.dng", mark(4));

    FilmStripFilterModel proxy;
    proxy.setSourceModel(&source);

    FilmStripFilter f;
    f.minRating = 3;
    proxy.setFilter(f);
    REQUIRE(visiblePaths(proxy) == QStringList{"/p/b.dng"});

    // Promote a.dng into range: the proxy should reveal it without a reset.
    source.setMarks("/p/a.dng", mark(5));
    REQUIRE(visiblePaths(proxy) == QStringList{"/p/a.dng", "/p/b.dng"});

    // Demote b.dng out of range: it should disappear.
    source.setMarks("/p/b.dng", mark(2));
    REQUIRE(visiblePaths(proxy) == QStringList{"/p/a.dng"});
}

TEST_CASE("clearing the filter restores all rows", "[filmstrip][filter]") {
    ensureApp();
    FilmStripModel source;
    source.setFiles({"/p/a.dng", "/p/b.dng"});
    source.setMarks("/p/a.dng", mark(1));
    source.setMarks("/p/b.dng", mark(4));

    FilmStripFilterModel proxy;
    proxy.setSourceModel(&source);

    FilmStripFilter f;
    f.minRating = 3;
    proxy.setFilter(f);
    REQUIRE(proxy.rowCount() == 1);

    proxy.setFilter(FilmStripFilter{});
    REQUIRE(proxy.rowCount() == 2);
}
