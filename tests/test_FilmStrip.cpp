#include "FilmStrip.h"
#include "TestApp.h"

#include <catch2/catch_test_macros.hpp>
#include <QDir>
#include <QFile>
#include <QListView>
#include <QItemSelectionModel>
#include <QMouseEvent>
#include <QTemporaryDir>

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

    delete strip;
}

TEST_CASE("FilmStrip: synthetic Ctrl+click mouse event adds to selection", "[filmstrip][multiselect]") {
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

    const QModelIndex second = listView->model()->index(1, 0);
    const QRect rect = listView->visualRect(second);
    if (!rect.isValid()) {
        WARN("visualRect not valid — item not laid out (offscreen?), skipping mouse test");
        delete strip;
        return;
    }

    // Simulate Ctrl+click by posting mouse press and release events
    const QPoint pos = rect.center();
    QMouseEvent press(QEvent::MouseButtonPress, pos, listView->viewport()->mapToGlobal(pos),
                      Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, listView->viewport()->mapToGlobal(pos),
                        Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(listView->viewport(), &press);
    QApplication::sendEvent(listView->viewport(), &release);
    qApp->processEvents();

    INFO("selectedIndexes: " << listView->selectionModel()->selectedIndexes().size());
    INFO("selectedPaths: " << strip->selectedPaths().size());
    CHECK(strip->selectedPaths().size() == 2);

    delete strip;
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

    delete strip;
}
