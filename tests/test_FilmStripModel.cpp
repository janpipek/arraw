#include "FilmStripModel.h"
#include "TestApp.h"
#include "UserMetadata.h"
#include <catch2/catch_test_macros.hpp>

#include <QImage>

// QAbstractListModel needs an application object for its meta-object machinery
// (dataChanged, QSignalSpy). Use the one shared QApplication: running the test
// binary directly executes every case in one process, so a separate
// QCoreApplication here would collide with the widget tests' QApplication.
namespace {
void ensureApp() {
    testApp();
}
} // namespace

TEST_CASE("model exposes the files it was given", "[filmstrip]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/photos/a.dng", "/photos/b.dng", "/photos/c.dng"});

    REQUIRE(model.rowCount() == 3);
    REQUIRE(model.data(model.index(0), Qt::DisplayRole).toString() == "a.dng");
}

TEST_CASE("model orders files by natural sort, not lexical", "[filmstrip]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/p/IMG_10.dng", "/p/IMG_2.dng", "/p/IMG_1.dng"});

    // Lexical sort would put IMG_10 before IMG_2; natural sort must not.
    REQUIRE(model.data(model.index(0), Qt::DisplayRole).toString() == "IMG_1.dng");
    REQUIRE(model.data(model.index(1), Qt::DisplayRole).toString() == "IMG_2.dng");
    REQUIRE(model.data(model.index(2), Qt::DisplayRole).toString() == "IMG_10.dng");
}

TEST_CASE("model exposes full path and a null thumbnail before load", "[filmstrip]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/photos/a.dng"});
    const QModelIndex idx = model.index(0);

    REQUIRE(model.data(idx, FilmStripModel::PathRole).toString() == "/photos/a.dng");
    REQUIRE(model.data(idx, Qt::DecorationRole).value<QImage>().isNull());
}

TEST_CASE("model exposes the companions attached to a primary", "[filmstrip]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/photos/a.cr2"}, {{"/photos/a.cr2", {"/photos/a.jpg"}}});

    const QStringList companions
        = model.data(model.index(0), FilmStripModel::CompanionsRole).toStringList();
    REQUIRE(companions == QStringList{"/photos/a.jpg"});
}

TEST_CASE("a primary with no companions reports an empty companion list", "[filmstrip]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/photos/a.dng"});
    REQUIRE(model.data(model.index(0), FilmStripModel::CompanionsRole).toStringList().isEmpty());
}

TEST_CASE("model offers the filename as the tooltip before metadata is loaded", "[filmstrip]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/photos/a.dng"});
    REQUIRE(model.data(model.index(0), Qt::ToolTipRole).toString() == "a.dng");
}

TEST_CASE("model formats cached metadata as the tooltip", "[filmstrip][tooltip]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/photos/a.dng"});

    ImageMetadata meta;
    meta.rows.append(qMakePair(QString("Model"), QString("Leica Q3")));
    meta.rows.append(qMakePair(QString("ISO"), QString("100")));

    model.setMetadata("/photos/a.dng", meta);

    REQUIRE(model.data(model.index(0), Qt::ToolTipRole).toString() == "a.dng\nLeica Q3\nISO 100");
}

TEST_CASE("indexForPath locates a known path and rejects an unknown one", "[filmstrip]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/p/b.dng", "/p/a.dng"}); // natural order: a(0), b(1)

    REQUIRE(model.indexForPath("/p/b.dng").row() == 1);
    REQUIRE_FALSE(model.indexForPath("/p/missing.dng").isValid());
}

TEST_CASE("rows report default marks until set", "[filmstrip][marks]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/p/a.dng"});
    const QModelIndex idx = model.index(0);

    REQUIRE(model.data(idx, FilmStripModel::RatingRole).toInt() == 0);
    REQUIRE(model.data(idx, FilmStripModel::LabelRole).toInt() == int(ColourLabel::None));
}

TEST_CASE("setting marks updates that row, keyed by path, and notifies", "[filmstrip][marks]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/p/a.dng", "/p/b.dng"}); // natural order: a, then b

    int changes = 0, topRow = -1;
    QList<int> roles;
    QObject::connect(
        &model,
        &QAbstractItemModel::dataChanged,
        [&](const QModelIndex& tl, const QModelIndex&, const QList<int>& r) {
            ++changes;
            topRow = tl.row();
            roles = r;
        });

    model.setMarks("/p/b.dng", {5, ColourLabel::Green});

    REQUIRE(model.data(model.index(1), FilmStripModel::RatingRole).toInt() == 5);
    REQUIRE(
        model.data(model.index(1), FilmStripModel::LabelRole).toInt() == int(ColourLabel::Green));
    REQUIRE(model.data(model.index(0), FilmStripModel::RatingRole).toInt() == 0);
    REQUIRE(changes == 1);
    REQUIRE(topRow == 1);
    REQUIRE(roles.contains(FilmStripModel::RatingRole));
    REQUIRE(roles.contains(FilmStripModel::LabelRole));
}

TEST_CASE("marks survive a re-sort because they are keyed by path", "[filmstrip][marks]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/p/a.dng", "/p/b.dng"});
    model.setMarks("/p/b.dng", {3, ColourLabel::Blue});

    // Re-assign the same files (a fresh scan); marks keyed by path persist.
    model.setFiles({"/p/b.dng", "/p/a.dng"});
    REQUIRE(model.data(model.indexForPath("/p/b.dng"), FilmStripModel::RatingRole).toInt() == 3);
}

TEST_CASE("setting a thumbnail updates that row and notifies the view", "[filmstrip]") {
    ensureApp();
    FilmStripModel model;
    model.setFiles({"/p/a.dng", "/p/b.dng"}); // natural order: a, then b
    // Direct-connected lambda fires synchronously on emit — no event loop needed.
    int changes = 0;
    int topRow = -1, bottomRow = -1;
    QObject::connect(
        &model,
        &QAbstractItemModel::dataChanged,
        [&](const QModelIndex& tl, const QModelIndex& br, const QList<int>&) {
            ++changes;
            topRow = tl.row();
            bottomRow = br.row();
        });

    QImage img(4, 4, QImage::Format_RGB32);
    img.fill(Qt::red);
    model.setThumbnail("/p/b.dng", img);

    REQUIRE(model.data(model.index(1), Qt::DecorationRole).value<QImage>().size() == QSize(4, 4));
    // The other row stays empty.
    REQUIRE(model.data(model.index(0), Qt::DecorationRole).value<QImage>().isNull());

    // Exactly one notification, naming only the changed row.
    REQUIRE(changes == 1);
    REQUIRE(topRow == 1);
    REQUIRE(bottomRow == 1);
}
