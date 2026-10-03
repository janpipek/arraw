#include "FolderWatcher.h"
#include "ShotFilterModel.h"
#include "ShotModel.h"
#include "support/Fixtures.h"
#include "support/TempDir.h"

#include <Sidecar.h>

#include <QCoreApplication>
#include <QPersistentModelIndex>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::app;

/// The film strip's shots: a model over a folder, a filter proxy over that, and a watcher that
/// keeps the first in step with the disk (src/app/ShotModel.h and neighbours).

namespace {

namespace fs = std::filesystem;

/// Runs the event loop until a condition holds; false if it does not within the time.
bool waitUntil(const std::function<bool()>& condition,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

/// Lets the model finish its reading.
bool settle(const ShotModel& model) {
    return waitUntil([&] { return model.pendingLoads() == 0; });
}

/// Puts a photograph with EXIF in a folder under a name.
fs::path addRaw(const test::TempDir& folder, const std::string& name) {
    const fs::path path = folder.file(name);
    fs::copy_file(test::fixture("exif-32x24.dng"), path);
    return path;
}

/// Puts an empty standard image in a folder; listing never opens it.
fs::path addJpeg(const test::TempDir& folder, const std::string& name) {
    const fs::path path = folder.file(name);
    std::ofstream(path, std::ios::binary).put('\0');
    return path;
}

/// Builds a filter without naming every field.
MarksFilter filterOf(int minRating, bool rejectsOnly = false, std::set<ColorLabel> labels = {}) {
    MarksFilter filter;
    filter.minRating = minRating;
    filter.rejectsOnly = rejectsOnly;
    filter.labels = std::move(labels);
    return filter;
}

QString display(const QAbstractItemModel& model, int row) {
    return model.index(row, 0).data(Qt::DisplayRole).toString();
}

} // namespace

TEST_CASE("The model lists the shots of a folder in natural order, with their roles",
          "[shotmodel]") {
    const test::TempDir folder;
    const fs::path ten = addRaw(folder, "IMG_10.dng");
    const fs::path two = addRaw(folder, "IMG_2.dng");
    addJpeg(folder, "IMG_2.jpg");
    ShotModel model;
    REQUIRE(model.rowCount() == 0);
    model.setFolder(folder.path());

    // Listed at once, before anything is read.
    REQUIRE(model.rowCount() == 2);
    REQUIRE(display(model, 0) == "IMG_2.dng");
    REQUIRE(display(model, 1) == "IMG_10.dng");
    const QModelIndex first = model.index(0);
    REQUIRE(first.data(ShotModel::PathRole).toString().toStdString() == two.string());
    REQUIRE(first.data(ShotModel::FormatLabelRole).toString() == "DNG+JPEG");
    REQUIRE(first.data(ShotModel::CompanionsRole).toStringList().size() == 1);
    REQUIRE_FALSE(first.data(ShotModel::MarksLoadedRole).toBool());
    REQUIRE(first.data(ShotModel::ThumbnailRole).value<QImage>().isNull());
    REQUIRE(model.rowOf(ten) == 1);
    REQUIRE(model.rowOf(folder.file("nothing.dng")) == -1);
}

TEST_CASE("Marks and EXIF arrive from the loader thread", "[shotmodel]") {
    const test::TempDir folder;
    const fs::path one = addRaw(folder, "IMG_1.dng");
    const fs::path two = addRaw(folder, "IMG_2.dng");
    writeSidecarMarks(two, {.rating = 4, .label = ColorLabel::Green});
    ShotModel model;
    model.setFolder(folder.path());
    REQUIRE(settle(model));

    const QModelIndex first = model.index(model.rowOf(one));
    const QModelIndex second = model.index(model.rowOf(two));
    REQUIRE(first.data(ShotModel::MarksLoadedRole).toBool());
    REQUIRE(first.data(ShotModel::RatingRole).toInt() == 0);
    REQUIRE(first.data(ShotModel::LabelRole).toInt() == -1);
    REQUIRE(second.data(ShotModel::RatingRole).toInt() == 4);
    REQUIRE(second.data(ShotModel::LabelRole).toInt() == static_cast<int>(ColorLabel::Green));
    REQUIRE(model.marks(1) == PhotoMarks{.rating = 4, .label = ColorLabel::Green});
    REQUIRE(model.exif(0));
    REQUIRE(model.exif(0)->model == "Fixture One");
    REQUIRE(first.data(Qt::ToolTipRole).toString() ==
            "IMG_1.dng\nDNG\n2024-05-01 10:00:00\nArraw Fixture One\nFixture 35mm F2.8\n"
            "ISO 400  1/250 s  f/2.8  35 mm");
}

TEST_CASE("Marks that cannot be read leave the defaults and do not stop the loading",
          "[shotmodel]") {
    const test::TempDir folder;
    addRaw(folder, "IMG_1.dng");
    std::ofstream(folder.file("IMG_1.xmp")) << "not xml";
    ShotModel model;
    model.setFolder(folder.path());
    REQUIRE(settle(model));
    REQUIRE(model.marks(0) == PhotoMarks{});
    REQUIRE(model.index(0).data(ShotModel::MarksLoadedRole).toBool());
}

TEST_CASE("Changing the folder drops what the loader was doing for the old one", "[shotmodel]") {
    const test::TempDir older;
    const test::TempDir newer;
    // The same names, so that a result of the old folder would land on a row of the new one.
    for (const char* name : {"IMG_1.dng", "IMG_2.dng", "IMG_3.dng", "IMG_4.dng"}) {
        const fs::path a = addRaw(older, name);
        const fs::path b = addRaw(newer, name);
        writeSidecarMarks(a, {.rating = 5});
        writeSidecarMarks(b, {.rating = 2});
    }
    ShotModel model;
    model.setFolder(older.path());
    model.setFolder(newer.path());
    REQUIRE(model.folder() == newer.path());
    REQUIRE(settle(model));
    for (int row = 0; row < model.rowCount(); ++row) {
        REQUIRE(model.marks(row).rating == 2);
    }
    // And nothing of the old one trickles in afterwards.
    waitUntil([] { return false; }, std::chrono::milliseconds(50));
    REQUIRE(model.pendingLoads() == 0);
    REQUIRE(model.rowOf(older.file("IMG_1.dng")) == -1);
}

TEST_CASE("A folder that cannot be read is refused and leaves the model as it was", "[shotmodel]") {
    const test::TempDir folder;
    addRaw(folder, "IMG_1.dng");
    ShotModel model;
    model.setFolder(folder.path());
    REQUIRE_THROWS_AS(model.setFolder(folder.file("missing")), std::runtime_error);
    REQUIRE(model.rowCount() == 1);
    REQUIRE(model.folder() == folder.path());
}

TEST_CASE("setMarks updates the row, and a reading under way cannot undo it", "[shotmodel]") {
    const test::TempDir folder;
    const fs::path one = addRaw(folder, "IMG_1.dng");
    ShotModel model;
    model.setFolder(folder.path());
    int changes = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                         if (roles.contains(ShotModel::RatingRole)) {
                             ++changes;
                         }
                     });
    // Before the loader has delivered anything (no events are processed yet).
    model.setMarks(one, {.rating = 3, .label = ColorLabel::Red});
    model.setMarks(folder.file("nothing.dng"), {.rating = 1});
    REQUIRE(changes == 1);
    REQUIRE(settle(model));
    REQUIRE(model.marks(0) == PhotoMarks{.rating = 3, .label = ColorLabel::Red});
    REQUIRE(model.index(0).data(ShotModel::RatingRole).toInt() == 3);
}

TEST_CASE("The thumbnail role is empty until a thumbnail is set", "[shotmodel]") {
    const test::TempDir folder;
    const fs::path one = addRaw(folder, "IMG_1.dng");
    ShotModel model;
    model.setFolder(folder.path());
    QImage thumbnail(4, 3, QImage::Format_RGB32);
    thumbnail.fill(Qt::red);
    model.setThumbnail(one, thumbnail);
    REQUIRE(model.index(0).data(ShotModel::ThumbnailRole).value<QImage>().size() == QSize(4, 3));
    model.setThumbnail(one, {});
    REQUIRE(model.index(0).data(ShotModel::ThumbnailRole).value<QImage>().isNull());
}

TEST_CASE("The tooltip says only what is known", "[shotmodel]") {
    const Shot shot{.primary = "/shoot/IMG_7.cr3", .companions = {"/shoot/IMG_7.jpg"}};
    REQUIRE(shotTooltip(shot, std::nullopt) == "IMG_7.cr3\nCR3+JPEG");
    REQUIRE(shotTooltip(shot, ExifInfo{}) == "IMG_7.cr3\nCR3+JPEG");

    ExifInfo exif;
    exif.make = "Canon";
    exif.model = "Canon EOS R5";
    exif.exposureTime = URational{3, 2};
    exif.fNumber = URational{11, 1};
    exif.focalLengthIn35mmFilm = 50;
    REQUIRE(shotTooltip(shot, exif) == "IMG_7.cr3\nCR3+JPEG\nCanon EOS R5\n1.5 s  f/11  50 mm");
}

TEST_CASE("The filter proxy shows a shot whose marks are not read yet", "[shotmodel][filter]") {
    const test::TempDir folder;
    const fs::path first = addRaw(folder, "IMG_1.dng");
    addRaw(folder, "IMG_2.dng");
    writeSidecarMarks(first, {.rating = 5});
    ShotModel model;
    ShotFilterModel proxy;
    proxy.setSourceModel(&model);
    model.setFolder(folder.path());
    // So that the view does not flicker as the loader catches up.
    proxy.setFilter(filterOf(5));
    REQUIRE(proxy.rowCount() == 2);
    REQUIRE(settle(model));
    REQUIRE(proxy.rowCount() == 1);
    REQUIRE(display(proxy, 0) == "IMG_1.dng");
}

TEST_CASE("The filter proxy shows the shots that pass a MarksFilter", "[shotmodel][filter]") {
    const test::TempDir folder;
    std::vector<fs::path> paths;
    for (const char* name : {"IMG_1.dng", "IMG_2.dng", "IMG_3.dng", "IMG_4.dng"}) {
        paths.push_back(addRaw(folder, name));
    }
    writeSidecarMarks(paths[0], {.rating = 5, .label = ColorLabel::Red});
    writeSidecarMarks(paths[1], {.rating = 2, .label = ColorLabel::Blue});
    writeSidecarMarks(paths[2], {.rating = -1});
    ShotModel model;
    ShotFilterModel proxy;
    proxy.setSourceModel(&model);
    model.setFolder(folder.path());

    REQUIRE(settle(model));
    REQUIRE(proxy.rowCount() == 4);

    SECTION("A threshold excludes rejects and unrated shots") {
        proxy.setFilter(filterOf(2));
        REQUIRE(proxy.rowCount() == 2);
        REQUIRE(display(proxy, 0) == "IMG_1.dng");
        REQUIRE(display(proxy, 1) == "IMG_2.dng");
    }
    SECTION("Rejects only") {
        proxy.setFilter(filterOf(0, true));
        REQUIRE(proxy.rowCount() == 1);
        REQUIRE(display(proxy, 0) == "IMG_3.dng");
    }
    SECTION("Labels combine by OR, and with the rating by AND") {
        proxy.setFilter(filterOf(0, false, {ColorLabel::Red, ColorLabel::Blue}));
        REQUIRE(proxy.rowCount() == 2);
        proxy.setFilter(filterOf(3, false, {ColorLabel::Red, ColorLabel::Blue}));
        REQUIRE(proxy.rowCount() == 1);
        REQUIRE(display(proxy, 0) == "IMG_1.dng");
    }
    SECTION("A change of marks is judged again") {
        proxy.setFilter(filterOf(4));
        REQUIRE(proxy.rowCount() == 1);
        model.setMarks(paths[0], {.rating = 1});
        REQUIRE(proxy.rowCount() == 0);
        model.setMarks(paths[3], {.rating = 4});
        REQUIRE(proxy.rowCount() == 1);
        REQUIRE(display(proxy, 0) == "IMG_4.dng");
        REQUIRE(proxy.nearestMatchingSourceRow(0) == 3);
    }
    SECTION("An inactive filter lets everything through, and a contradictory one is refused") {
        proxy.setFilter(filterOf(5));
        proxy.setFilter({});
        REQUIRE(proxy.rowCount() == 4);
        REQUIRE_THROWS_AS(proxy.setFilter(filterOf(1, true)), std::invalid_argument);
        REQUIRE(proxy.filter() == MarksFilter{});
    }
}

TEST_CASE("The nearest matching row looks forward first, then back", "[shotmodel][filter]") {
    const auto among = [](std::vector<int> rows) {
        return [rows = std::move(rows)](int row) {
            return std::ranges::find(rows, row) != rows.end();
        };
    };
    SECTION("A row that matches stays") {
        REQUIRE(nearestMatchingRow(6, 2, among({0, 2, 5})) == 2);
    }
    SECTION("The next match after the row, even when an earlier one is closer") {
        REQUIRE(nearestMatchingRow(10, 4, among({3, 7})) == 7);
    }
    SECTION("The last match before the row when none follows") {
        REQUIRE(nearestMatchingRow(10, 8, among({1, 3})) == 3);
    }
    SECTION("A row past the end counts as the end") {
        REQUIRE(nearestMatchingRow(4, 9, among({1})) == 1);
        REQUIRE(nearestMatchingRow(4, -3, among({2})) == 2);
    }
    SECTION("Nothing matches") {
        REQUIRE_FALSE(nearestMatchingRow(5, 2, among({})));
        REQUIRE_FALSE(nearestMatchingRow(0, 0, among({0})));
    }
}

TEST_CASE("The watcher adds and removes shots as files come and go", "[shotmodel][watcher]") {
    const test::TempDir folder;
    const fs::path one = addRaw(folder, "IMG_1.dng");
    const fs::path three = addRaw(folder, "IMG_3.dng");
    writeSidecarMarks(three, {.rating = 4});
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    model.setFolder(folder.path());
    REQUIRE(settle(model));
    const QPersistentModelIndex tracked = model.index(model.rowOf(three));
    REQUIRE(tracked.row() == 1);

    // A new file appears between the two, the selection stays on its shot.
    const fs::path two = addRaw(folder, "IMG_2.dng");
    REQUIRE(waitUntil([&] { return model.rowCount() == 3; }));
    REQUIRE(settle(model));
    REQUIRE(model.rowOf(two) == 1);
    REQUIRE(tracked.row() == 2);
    REQUIRE(tracked.data(ShotModel::PathRole).toString().toStdString() == three.string());
    // Its marks are kept, and the new shot's were read.
    REQUIRE(model.marks(2).rating == 4);
    REQUIRE(model.index(1).data(ShotModel::MarksLoadedRole).toBool());
    REQUIRE(model.exif(1));

    // A JPEG joins a shot, which changes its label and nothing else.
    addJpeg(folder, "IMG_1.jpg");
    REQUIRE(waitUntil(
        [&] { return model.index(0).data(ShotModel::FormatLabelRole).toString() == "DNG+JPEG"; }));
    REQUIRE(model.rowCount() == 3);

    // The RAW goes, and its JPEG stands alone.
    fs::remove(one);
    const fs::path jpeg = folder.file("IMG_1.jpg");
    REQUIRE(waitUntil([&] { return model.rowOf(one) == -1 && model.rowOf(jpeg) == 0; }));
    REQUIRE(model.rowCount() == 3);
    REQUIRE(model.index(0).data(ShotModel::FormatLabelRole).toString() == "JPEG");

    fs::remove(jpeg);
    REQUIRE(waitUntil([&] { return model.rowCount() == 2; }));
    REQUIRE(tracked.row() == 1);
    REQUIRE(settle(model));
}

TEST_CASE("The watcher reports a sidecar another program changed, and not the application's own",
          "[shotmodel][watcher]") {
    const test::TempDir folder;
    const fs::path one = addRaw(folder, "IMG_1.dng");
    const fs::path two = addRaw(folder, "IMG_2.dng");
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    std::vector<std::string> external;
    QObject::connect(&watcher, &FolderWatcher::sidecarChangedExternally, &watcher,
                     [&](const QString& primary) { external.push_back(primary.toStdString()); });
    int refreshed = 0;
    QObject::connect(&model, &ShotModel::sidecarRefreshed, &model,
                     [&](const QString&, qint64) { ++refreshed; });
    model.setFolder(folder.path());
    REQUIRE(settle(model));

    SECTION("An own write, noted right after it") {
        writeSidecarMarks(one, {.rating = 5});
        model.setMarks(one, {.rating = 5});
        watcher.noteOwnWrite(sidecarPath(one));
        REQUIRE(waitUntil([&] { return refreshed == 1; }));
        REQUIRE(settle(model));
        REQUIRE(external.empty());
        REQUIRE(model.marks(0).rating == 5);
    }
    SECTION("Another program's write") {
        writeSidecarMarks(two, {.rating = -1, .label = ColorLabel::Purple});
        REQUIRE(waitUntil([&] { return !external.empty(); }));
        REQUIRE(external == std::vector<std::string>{two.string()});
        REQUIRE(model.marks(1) == PhotoMarks{.rating = -1, .label = ColorLabel::Purple});
        // The first shot's sidecar is untouched, so it is not reported.
        REQUIRE(settle(model));
        REQUIRE(model.marks(0) == PhotoMarks{});
    }
    SECTION("An own write, then another program's over it") {
        writeSidecarMarks(one, {.rating = 5});
        model.setMarks(one, {.rating = 5});
        watcher.noteOwnWrite(sidecarPath(one));
        REQUIRE(waitUntil([&] { return refreshed == 1; }));
        REQUIRE(external.empty());
        writeSidecarMarks(one, {.rating = 1});
        REQUIRE(waitUntil([&] { return !external.empty(); }));
        REQUIRE(model.marks(0).rating == 1);
    }
}
