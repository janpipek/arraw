#include "FolderWatcher.h"
#include "ShotFilterModel.h"
#include "ShotModel.h"
#include "support/Fixtures.h"
#include "support/ListingFeed.h"
#include "support/TempDir.h"

#include <Sidecar.h>

#include <QCoreApplication>
#include <QImage>
#include <QPersistentModelIndex>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
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
/// keeps the first in step with the disk (src/app/ShotModel.h and neighbours). The model reads no
/// folder itself: without a watcher, test::listNow answers each walk it asks for at once.

namespace {

namespace fs = std::filesystem;

/// A relative path as the tooltip shows it: with the separator of the platform.
QString native(const char* relative) {
    return QString::fromStdU16String(fs::path(relative).make_preferred().u16string());
}

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

/// Lets the model finish its listing, its refresh and its reading.
bool settle(const ShotModel& model) {
    return waitUntil(
        [&] { return !model.isListing() && !model.isRefreshing() && model.pendingLoads() == 0; });
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
    test::listNow(model);

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
    test::listNow(model);
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
    test::listNow(model);
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
    test::listNow(model);
    model.setFolder(newer.path());
    test::listNow(model);
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
    test::listNow(model);
    REQUIRE_THROWS_AS(model.setFolder(folder.file("missing")), std::runtime_error);
    REQUIRE(model.rowCount() == 1);
    REQUIRE(model.folder() == folder.path());
}

TEST_CASE("setMarks updates the row, and a reading under way cannot undo it", "[shotmodel]") {
    const test::TempDir folder;
    const fs::path one = addRaw(folder, "IMG_1.dng");
    ShotModel model;
    model.setFolder(folder.path());
    test::listNow(model);
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
    test::listNow(model);
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
    test::listNow(model);
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
    test::listNow(model);

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

TEST_CASE("The sidecar role says whether a sidecar file exists", "[shotmodel][sidecar]") {
    const test::TempDir folder;
    const fs::path one = addRaw(folder, "IMG_1.dng");
    const fs::path two = addRaw(folder, "IMG_2.dng");
    writeSidecarMarks(two, {.rating = 2});
    ShotModel model;
    std::vector<int> changes;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&](const QModelIndex& index, const QModelIndex&, const QList<int>& roles) {
                         if (roles.contains(ShotModel::HasSidecarRole)) {
                             changes.push_back(index.row());
                         }
                     });
    model.setFolder(folder.path());
    test::listNow(model);
    // False until the marks are read.
    REQUIRE_FALSE(model.index(1).data(ShotModel::HasSidecarRole).toBool());
    REQUIRE(settle(model));
    REQUIRE_FALSE(model.index(0).data(ShotModel::HasSidecarRole).toBool());
    REQUIRE(model.index(1).data(ShotModel::HasSidecarRole).toBool());
    REQUIRE_FALSE(model.index(0).data(Qt::ToolTipRole).toString().contains("sidecar"));
    REQUIRE(model.index(1).data(Qt::ToolTipRole).toString().contains("XMP sidecar"));
    REQUIRE(changes == std::vector<int>{1});

    SECTION("A sidecar that appears and disappears") {
        writeSidecarMarks(one, {.rating = 4});
        model.refresh();
        test::listNow(model);
        REQUIRE(waitUntil([&] { return model.index(0).data(ShotModel::HasSidecarRole).toBool(); }));
        fs::remove(sidecarPath(two));
        model.refresh();
        test::listNow(model);
        REQUIRE(
            waitUntil([&] { return !model.index(1).data(ShotModel::HasSidecarRole).toBool(); }));
        REQUIRE(settle(model));
    }
    SECTION("A write through the model") {
        writeSidecarMarks(one, {.rating = 5});
        model.setMarks(one, {.rating = 5});
        REQUIRE(model.index(0).data(ShotModel::HasSidecarRole).toBool());
        // Without a file, setting marks shows no sidecar.
        model.noteSidecarWritten(folder.file("nothing.dng"));
        fs::remove(sidecarPath(one));
        model.noteSidecarWritten(one);
        REQUIRE_FALSE(model.index(0).data(ShotModel::HasSidecarRole).toBool());
    }
}

TEST_CASE("A recursive model lists the subfolders and names them in the tooltip",
          "[shotmodel][recursive]") {
    const test::TempDir folder;
    fs::create_directories(folder.file("a"));
    fs::create_directories(folder.file("b"));
    const fs::path top = addRaw(folder, "IMG_1.dng");
    const fs::path inA = addRaw(folder, "a/IMG_1.dng");
    const fs::path inB = addRaw(folder, "b/IMG_1.dng");
    ShotModel model;
    model.setFolder(folder.path());
    test::listNow(model);
    REQUIRE(model.rowCount() == 1);
    REQUIRE_FALSE(model.recursive());

    model.setFolder(folder.path(), true);
    test::listNow(model);
    REQUIRE(model.recursive());
    REQUIRE(model.rowCount() == 3);
    REQUIRE(model.rowOf(top) == 0);
    REQUIRE(model.rowOf(inA) == 1);
    REQUIRE(model.rowOf(inB) == 2);
    REQUIRE(settle(model));
    REQUIRE(model.index(0).data(Qt::ToolTipRole).toString().startsWith("IMG_1.dng\n"));
    REQUIRE(
        model.index(1).data(Qt::ToolTipRole).toString().startsWith(native("a/IMG_1.dng") + "\n"));
    REQUIRE(model.index(2).data(Qt::DisplayRole).toString() == "IMG_1.dng");
}

TEST_CASE("The display role is the bare file name, also in a subfolder", "[shotmodel][recursive]") {
    const test::TempDir folder;
    fs::create_directories(folder.file("day1/raw"));
    addRaw(folder, "day1/raw/IMG_0042.dng");
    ShotModel model;
    model.setFolder(folder.path(), true);
    test::listNow(model);
    REQUIRE(model.rowCount() == 1);
    CHECK(display(model, 0) == "IMG_0042.dng");
    CHECK(model.index(0)
              .data(Qt::ToolTipRole)
              .toString()
              .startsWith(native("day1/raw/IMG_0042.dng")));
}

TEST_CASE("The watcher sees a file added in a sub-directory, and in a new one",
          "[shotmodel][watcher][recursive]") {
    const test::TempDir folder;
    fs::create_directories(folder.file("sub"));
    addRaw(folder, "IMG_1.dng");
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    model.setFolder(folder.path(), true);
    REQUIRE(settle(model));
    REQUIRE(model.rowCount() == 1);

    const fs::path inSub = addRaw(folder, "sub/IMG_2.dng");
    REQUIRE(waitUntil([&] { return model.rowOf(inSub) >= 0; }));

    // A sub-directory made later joins the watch once the model has refreshed; the file goes
    // in only when it does, so that only the watch can show it.
    fs::create_directories(folder.file("later"));
    const QString later = QString::fromStdU16String(folder.file("later").u16string());
    REQUIRE(waitUntil([&] { return watcher.watchedDirectories().contains(later); }));
    const fs::path inLater = addRaw(folder, "later/IMG_3.dng");
    REQUIRE(waitUntil([&] { return model.rowOf(inLater) >= 0; }));
    REQUIRE(settle(model));
}

TEST_CASE("The watcher watches the folders the listing enters and no others",
          "[shotmodel][watcher][recursive]") {
    const test::TempDir folder;
    for (const char* name : {"real/deep", ".hidden", "@eaDir/IMG_1.dng", "#recycle", "$RECYCLE.BIN",
                             "System Volume Information"}) {
        fs::create_directories(folder.file(name));
    }
    addRaw(folder, "IMG_1.dng");
    std::error_code error;
    fs::create_directory_symlink(folder.file("real"), folder.file("link"), error);
    ShotModel model;
    FolderWatcher watcher(model, std::chrono::milliseconds(20));
    model.setFolder(folder.path(), true);
    // Watched once the walk that finds them is over.
    REQUIRE(waitUntil([&] { return !model.isListing(); }));

    std::set<fs::path> expected{folder.path(), folder.file("real"), folder.file("real/deep")};
    std::set<fs::path> watched;
    for (const QString& entry : watcher.watchedDirectories()) {
        watched.insert(fs::path(entry.toStdU16String()));
    }
    REQUIRE(watched == expected);
}

namespace {

/// Records the row signals of a model as text such as `+3..4` and `-1..2`.
struct SignalLog {
    std::vector<std::string> events;
    int resets = 0;

    explicit SignalLog(ShotModel& model) {
        QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                         [this](const QModelIndex&, int first, int last) {
                             events.push_back("+" + std::to_string(first) + ".." +
                                              std::to_string(last));
                         });
        QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                         [this](const QModelIndex&, int first, int last) {
                             events.push_back("-" + std::to_string(first) + ".." +
                                              std::to_string(last));
                         });
        QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [this] { ++resets; });
    }
};

/// Names of the primaries in row order.
std::vector<std::string> names(const ShotModel& model) {
    std::vector<std::string> result;
    for (int row = 0; row < model.rowCount(); ++row) {
        result.push_back(model.shot(row).primary.filename().string());
    }
    return result;
}

} // namespace

TEST_CASE("A refresh signals one insert or remove per run of rows", "[shotmodel][merge]") {
    const test::TempDir folder;
    for (const char* name :
         {"IMG_01.jpg", "IMG_02.jpg", "IMG_04.jpg", "IMG_05.jpg", "IMG_07.jpg", "IMG_08.jpg"}) {
        addJpeg(folder, name);
    }
    ShotModel model;
    model.setFolder(folder.path());
    test::listNow(model);
    REQUIRE(settle(model));
    SignalLog log(model);

    SECTION("Inserts") {
        for (const char* name : {"IMG_03.jpg", "IMG_06.jpg", "IMG_09.jpg", "IMG_10.jpg"}) {
            addJpeg(folder, name);
        }
        model.refresh();
        test::listNow(model);
        REQUIRE(log.events == std::vector<std::string>{"+2..2", "+5..5", "+8..9"});
        REQUIRE(model.rowCount() == 10);
        REQUIRE(model.rowOf(folder.file("IMG_10.jpg")) == 9);
        REQUIRE(model.rowOf(folder.file("IMG_07.jpg")) == 6);
        REQUIRE(log.resets == 0);
    }
    SECTION("Removes, the last run first") {
        for (const char* name : {"IMG_02.jpg", "IMG_04.jpg", "IMG_05.jpg", "IMG_08.jpg"}) {
            fs::remove(folder.file(name));
        }
        model.refresh();
        test::listNow(model);
        REQUIRE(log.events == std::vector<std::string>{"-5..5", "-1..3"});
        REQUIRE(names(model) == std::vector<std::string>{"IMG_01.jpg", "IMG_07.jpg"});
        REQUIRE(model.rowOf(folder.file("IMG_07.jpg")) == 1);
        REQUIRE(model.rowOf(folder.file("IMG_04.jpg")) == -1);
    }
    SECTION("A change that touches nothing sends nothing") {
        model.refresh();
        test::listNow(model);
        REQUIRE(log.events.empty());
        REQUIRE(model.pendingLoads() >= 0);
    }
}

TEST_CASE("Rows keep their marks, EXIF and thumbnail through a merge", "[shotmodel][merge]") {
    const test::TempDir folder;
    const fs::path one = addRaw(folder, "IMG_1.dng");
    const fs::path three = addRaw(folder, "IMG_3.dng");
    const fs::path five = addRaw(folder, "IMG_5.dng");
    ShotModel model;
    model.setFolder(folder.path());
    test::listNow(model);
    REQUIRE(settle(model));
    model.setMarks(three, {.rating = 4, .label = ColorLabel::Red});
    QImage thumbnail(4, 3, QImage::Format_RGB32);
    thumbnail.fill(Qt::green);
    model.setThumbnail(five, thumbnail);
    const QPersistentModelIndex tracked = model.index(model.rowOf(five));

    const fs::path two = addRaw(folder, "IMG_2.dng");
    fs::remove(one);
    model.refresh();
    test::listNow(model);
    REQUIRE(names(model) == std::vector<std::string>{"IMG_2.dng", "IMG_3.dng", "IMG_5.dng"});
    REQUIRE(tracked.row() == 2);
    REQUIRE(model.marks(model.rowOf(three)) == PhotoMarks{.rating = 4, .label = ColorLabel::Red});
    REQUIRE(model.exif(model.rowOf(three)));
    REQUIRE(model.index(model.rowOf(five)).data(ShotModel::ThumbnailRole).value<QImage>() ==
            thumbnail);
    REQUIRE(settle(model));
    REQUIRE(model.exif(model.rowOf(two)));
    REQUIRE(model.marks(model.rowOf(three)).rating == 4);
}

TEST_CASE("A companion that joins is an update in place", "[shotmodel][merge]") {
    const test::TempDir folder;
    addRaw(folder, "IMG_1.dng");
    addRaw(folder, "IMG_2.dng");
    ShotModel model;
    model.setFolder(folder.path());
    test::listNow(model);
    REQUIRE(settle(model));
    SignalLog log(model);
    std::vector<int> changedRows;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&](const QModelIndex& index, const QModelIndex&, const QList<int>& roles) {
                         if (roles.contains(ShotModel::FormatLabelRole)) {
                             changedRows.push_back(index.row());
                         }
                     });
    addJpeg(folder, "IMG_2.jpg");
    model.refresh();
    test::listNow(model);
    REQUIRE(log.events.empty());
    REQUIRE(changedRows == std::vector<int>{1});
    REQUIRE(model.index(1).data(ShotModel::FormatLabelRole).toString() == "DNG+JPEG");
}

TEST_CASE("Folders are merged where they stand: new, vanished and changed ones",
          "[shotmodel][merge][recursive]") {
    const test::TempDir folder;
    for (const char* name : {"b", "d"}) {
        fs::create_directories(folder.file(name));
    }
    addJpeg(folder, "IMG_1.jpg");
    addJpeg(folder, "b/IMG_1.jpg");
    addJpeg(folder, "b/IMG_2.jpg");
    addJpeg(folder, "d/IMG_1.jpg");
    ShotModel model;
    model.setFolder(folder.path(), true);
    test::listNow(model);
    REQUIRE(settle(model));
    REQUIRE(model.rowCount() == 4);
    SignalLog log(model);

    // A folder between the two, one that goes, one that gains a shot.
    fs::create_directories(folder.file("c/deep"));
    addJpeg(folder, "c/IMG_1.jpg");
    addJpeg(folder, "c/deep/IMG_1.jpg");
    fs::remove_all(folder.file("d"));
    addJpeg(folder, "b/IMG_3.jpg");
    model.refresh();
    test::listNow(model);
    REQUIRE(settle(model));

    REQUIRE(model.rowCount() == 6);
    const std::vector<std::string> expected{"IMG_1.jpg", "IMG_1.jpg", "IMG_2.jpg",
                                            "IMG_3.jpg", "IMG_1.jpg", "IMG_1.jpg"};
    REQUIRE(names(model) == expected);
    const std::vector<fs::path> order{folder.file("IMG_1.jpg"),   folder.file("b/IMG_1.jpg"),
                                      folder.file("b/IMG_2.jpg"), folder.file("b/IMG_3.jpg"),
                                      folder.file("c/IMG_1.jpg"), folder.file("c/deep/IMG_1.jpg")};
    for (int row = 0; row < 6; ++row) {
        REQUIRE(model.shot(row).primary == order[static_cast<std::size_t>(row)]);
        REQUIRE(model.rowOf(order[static_cast<std::size_t>(row)]) == row);
    }
    REQUIRE(model.rowOf(folder.file("d/IMG_1.jpg")) == -1);
    REQUIRE(log.resets == 0);
    // One signal per run: b gains a row, c and c/deep, new and next to each other, come in as
    // one run, and d, which was last, goes.
    REQUIRE(log.events == std::vector<std::string>{"+3..3", "+4..5", "-6..6"});
}

TEST_CASE("A scattered change resets the model and keeps what the rows know",
          "[shotmodel][merge]") {
    const test::TempDir folder;
    const auto name = [](int n) {
        char text[32];
        std::snprintf(text, sizeof text, "IMG_%04d.jpg", n);
        return std::string(text);
    };
    for (int n = 0; n < 200; n += 2) {
        addJpeg(folder, name(n));
    }
    ShotModel model;
    model.setFolder(folder.path());
    test::listNow(model);
    REQUIRE(settle(model));
    model.setMarks(folder.file(name(100)), {.rating = 3});
    QImage thumbnail(2, 2, QImage::Format_RGB32);
    thumbnail.fill(Qt::blue);
    model.setThumbnail(folder.file(name(100)), thumbnail);
    SignalLog log(model);

    // The odd ones come in between all of them: 100 insert runs.
    for (int n = 1; n < 200; n += 2) {
        addJpeg(folder, name(n));
    }
    model.refresh();
    test::listNow(model);
    REQUIRE(log.resets == 1);
    REQUIRE(log.events.empty());
    REQUIRE(model.rowCount() == 200);
    REQUIRE(model.rowOf(folder.file(name(100))) == 100);
    REQUIRE(model.rowOf(folder.file(name(199))) == 199);
    REQUIRE(model.marks(100).rating == 3);
    REQUIRE(model.index(100).data(ShotModel::ThumbnailRole).value<QImage>() == thumbnail);
    // The 100 new shots are loaded, and the old ones checked.
    REQUIRE(settle(model));
    REQUIRE(model.index(101).data(ShotModel::MarksLoadedRole).toBool());

    // Exactly 64 runs are still merged by runs.
    log.resets = 0;
    for (int n = 0; n < 128; n += 2) {
        fs::remove(folder.file(name(n)));
    }
    model.refresh();
    test::listNow(model);
    REQUIRE(log.resets == 0);
    REQUIRE(log.events.size() == 64);
    REQUIRE(model.rowCount() == 200 - 64);
}

TEST_CASE("A merged folder's sidecars are checked when it reported a change, and only then",
          "[shotmodel][merge][recursive]") {
    const test::TempDir folder;
    fs::create_directories(folder.file("a"));
    fs::create_directories(folder.file("b"));
    for (const char* name : {"a/IMG_1.jpg", "a/IMG_2.jpg", "a/IMG_3.jpg", "b/IMG_1.jpg",
                             "b/IMG_2.jpg", "b/IMG_3.jpg", "b/IMG_4.jpg"}) {
        addJpeg(folder, name);
    }
    ShotModel model;
    model.setFolder(folder.path(), true);
    test::listNow(model);
    REQUIRE(settle(model));
    // A directory read again after the walk, as the watcher hands it over.
    const auto reread = [&](const char* name, bool recheck) {
        const fs::path directory = folder.file(name);
        model.applyFolders(model.walk(), {{.folder = directory,
                                           .shots = listFolder(directory).shots,
                                           .gone = false,
                                           .recheck = recheck}});
    };

    SECTION("Read again without a report and unchanged: nothing is read") {
        reread("a", false);
        REQUIRE(model.pendingLoads() == 0);
    }
    SECTION("A report checks that folder's rows only") {
        reread("a", true);
        REQUIRE(model.pendingLoads() == 3);
        REQUIRE(settle(model));
    }
    SECTION("A changed folder is checked, and so are the new rows' first reads") {
        addJpeg(folder, "a/IMG_4.jpg");
        reread("a", false);
        // Three rechecks in a, then marks and EXIF of the new shot.
        REQUIRE(model.pendingLoads() == 3 + 2);
        REQUIRE(model.rowCount() == 8);
        REQUIRE(settle(model));
    }
    SECTION("A folder that went checks no one, and takes its rows") {
        model.removeSubtree(model.walk(), folder.file("b"));
        REQUIRE(model.pendingLoads() == 0);
        REQUIRE(model.rowCount() == 3);
    }
    SECTION("A refresh of all checks every loaded row") {
        model.refresh();
        test::listNow(model);
        REQUIRE(model.pendingLoads() == 7);
        REQUIRE(settle(model));
    }
}
