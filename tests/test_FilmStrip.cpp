#include "FilmStrip.h"
#include "FilmStripModel.h"
#include "TestApp.h"

#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QFile>
#include <QListView>
#include <QItemSelectionModel>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThreadPool>

namespace {

FilmStrip* makeStripWithFiles(const QTemporaryDir& dir, int n) {
    for (int i = 1; i <= n; ++i)
        { QFile f(dir.filePath(QString("IMG_000%1.jpg").arg(i))); (void)f.open(QIODevice::WriteOnly); }
    auto* strip = new FilmStrip;
    strip->resize(800, 100);
    strip->show();
    strip->setDirectory(dir.path());
    qApp->processEvents();
    return strip;
}

// The strip kicks off background thumbnail/marks decodes that capture the cache
// and post results back via queued invokeMethod. Deleting it mid-flight would
// dereference freed memory, so drain the pool and queued events before teardown.
void destroyStrip(FilmStrip* strip) {
    QThreadPool::globalInstance()->waitForDone();
    qApp->processEvents();
    delete strip;
}

} // namespace

TEST_CASE("FilmStrip: programmatic multi-select populates selectedPaths", "[filmstrip][multiselect]") {
    testApp();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto* strip = makeStripWithFiles(dir, 4);

    strip->selectFirst();
    qApp->processEvents();

    REQUIRE(strip->selectedPaths().size() == 1);

    auto* listView = strip->findChild<QListView*>();
    REQUIRE(listView != nullptr);

    // Add second item to selection (Ctrl+click equivalent at the model level)
    QModelIndex second = listView->model()->index(1, 0);
    listView->selectionModel()->select(second, QItemSelectionModel::Select);
    qApp->processEvents();

    CHECK(listView->selectionModel()->selectedIndexes().size() == 2);
    CHECK(strip->selectedPaths().size() == 2);

    destroyStrip(strip);
}

namespace {

// Returns false (with a WARN) when the item has no laid-out geometry, e.g. on the
// offscreen platform where the strip never gets a real layout pass.
bool clickItem(QListView* view, int row, Qt::KeyboardModifiers mods) {
    const QModelIndex idx = view->model()->index(row, 0);
    const QRect rect = view->visualRect(idx);
    if (!rect.isValid())
        return false;
    const QPoint pos = rect.center();
    const QPoint global = view->viewport()->mapToGlobal(pos);
    QMouseEvent press(QEvent::MouseButtonPress, pos, global,
                      Qt::LeftButton, Qt::LeftButton, mods);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, global,
                        Qt::LeftButton, Qt::LeftButton, mods);
    QApplication::sendEvent(view->viewport(), &press);
    QApplication::sendEvent(view->viewport(), &release);
    qApp->processEvents();
    return true;
}

} // namespace

TEST_CASE("FilmStrip: Ctrl+click adds to selection but keeps the active image", "[filmstrip][multiselect]") {
    testApp();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto* strip = makeStripWithFiles(dir, 4);

    strip->selectFirst();
    qApp->processEvents();

    auto* listView = strip->findChild<QListView*>();
    REQUIRE(listView != nullptr);
    listView->resize(800, 100);
    qApp->processEvents();

    const QModelIndex activeBefore = listView->currentIndex();
    REQUIRE(activeBefore.row() == 0);

    if (!clickItem(listView, 1, Qt::ControlModifier)) {
        WARN("visualRect not valid — item not laid out (offscreen?), skipping mouse test");
        destroyStrip(strip);
        return;
    }

    // Both files are now batch targets...
    CHECK(strip->selectedPaths().size() == 2);
    // ...but the active image (current index / viewport target) must NOT move.
    CHECK(listView->currentIndex().row() == 0);

    destroyStrip(strip);
}

TEST_CASE("FilmStrip: Ctrl+click never deselects the active image", "[filmstrip][multiselect]") {
    testApp();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto* strip = makeStripWithFiles(dir, 4);

    strip->selectFirst();
    qApp->processEvents();

    auto* listView = strip->findChild<QListView*>();
    REQUIRE(listView != nullptr);
    listView->resize(800, 100);
    qApp->processEvents();

    // Ctrl+click the active item itself: toggle would drop it, but it must stay.
    if (!clickItem(listView, 0, Qt::ControlModifier)) {
        WARN("visualRect not valid — skipping");
        destroyStrip(strip);
        return;
    }

    CHECK(strip->selectedPaths().contains(
        listView->model()->index(0, 0).data(FilmStripModel::PathRole).toString()));
    CHECK(listView->currentIndex().row() == 0);

    destroyStrip(strip);
}

TEST_CASE("FilmStrip: Shift+click selects a contiguous range, active pinned", "[filmstrip][multiselect]") {
    testApp();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto* strip = makeStripWithFiles(dir, 5);

    strip->selectFirst();
    qApp->processEvents();

    auto* listView = strip->findChild<QListView*>();
    REQUIRE(listView != nullptr);
    listView->resize(1000, 100);
    qApp->processEvents();

    if (!clickItem(listView, 3, Qt::ShiftModifier)) {
        WARN("visualRect not valid — skipping");
        destroyStrip(strip);
        return;
    }

    // Rows 0..3 inclusive = 4 files.
    CHECK(strip->selectedPaths().size() == 4);
    CHECK(listView->currentIndex().row() == 0);

    destroyStrip(strip);
}

TEST_CASE("FilmStrip: selectFirst collapses any prior multi-selection", "[filmstrip][multiselect]") {
    testApp();

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto* strip = makeStripWithFiles(dir, 4);

    strip->selectFirst();
    qApp->processEvents();

    auto* listView = strip->findChild<QListView*>();
    REQUIRE(listView != nullptr);

    // Add two items
    listView->selectionModel()->select(listView->model()->index(1, 0), QItemSelectionModel::Select);
    listView->selectionModel()->select(listView->model()->index(2, 0), QItemSelectionModel::Select);
    qApp->processEvents();
    REQUIRE(strip->selectedPaths().size() == 3);

    // selectFirst should collapse back to 1
    strip->selectFirst();
    qApp->processEvents();
    CHECK(strip->selectedPaths().size() == 1);

    destroyStrip(strip);
}
