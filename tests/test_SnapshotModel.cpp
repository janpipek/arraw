#include "HistoryModel.h"
#include "SnapshotModel.h"

#include <DevelopState.h>
#include <EditSession.h>
#include <Snapshots.h>

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QImage>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>

using namespace arraw;
using namespace arraw::app;

/// The snapshot tree of the History dock: its shape, roles and change notifications
/// (src/app/SnapshotModel.h).

namespace {

/// A JPEG of one colour, as a thumbnail.
SnapshotThumbnail thumbnailOf(const QColor& colour, int width, int height, std::string key) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(colour);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    REQUIRE(image.save(&buffer, "JPEG", 90));
    SnapshotThumbnail thumbnail;
    thumbnail.jpeg.resize(static_cast<std::size_t>(bytes.size()));
    std::memcpy(thumbnail.jpeg.data(), bytes.constData(), thumbnail.jpeg.size());
    thumbnail.renderKey = std::move(key);
    return thumbnail;
}

Snapshot snap(std::uint64_t id, std::optional<std::uint64_t> parent, std::string name,
              float exposure) {
    Snapshot snapshot;
    snapshot.id = SnapshotId{id};
    if (parent) {
        snapshot.parent = SnapshotId{*parent};
    }
    snapshot.name = std::move(name);
    snapshot.created = std::chrono::sys_seconds{std::chrono::seconds{1'790'000'000 + 60 * id}};
    snapshot.state.settings.tone.exposure = exposure;
    return snapshot;
}

/// Roots 1 and 2; 3 below 1; 4 below 3.
SnapshotCollection forest() {
    SnapshotCollection collection;
    collection.entries = {snap(1, {}, "Start", 0.0F), snap(2, {}, "Other", 1.0F),
                          snap(3, 1, "Warm", 0.5F), snap(4, 3, "Warmer", 1.5F)};
    // Below Warm, 4 differs in the contrast alone; below Start it would differ in two settings.
    collection.entries[3].state.settings.tone.exposure = 0.5F;
    collection.entries[3].state.settings.tone.contrast = 10.0F;
    collection.nextId = 5;
    collection.revision = 4;
    return collection;
}

QString text(const SnapshotModel& model, const QModelIndex& index, int role = Qt::DisplayRole) {
    return model.data(index, role).toString();
}

/// Counts the resets and the changed rows a model announces.
struct Watch {
    int resets = 0;
    int changes = 0;

    explicit Watch(SnapshotModel& model) {
        QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [this] { ++resets; });
        QObject::connect(&model, &QAbstractItemModel::dataChanged, &model, [this] { ++changes; });
    }
};

} // namespace

TEST_CASE("The tree has roots and nested children in collection order", "[app][snapshots]") {
    SnapshotModel model;
    model.setSnapshots(forest(), std::nullopt, std::nullopt);

    REQUIRE(model.rowCount() == 2);
    const QModelIndex start = model.index(0, 0);
    const QModelIndex other = model.index(1, 0);
    CHECK(text(model, start) == "Start");
    CHECK(text(model, other) == "Other");
    CHECK(model.rowCount(other) == 0);

    REQUIRE(model.rowCount(start) == 1);
    const QModelIndex warm = model.index(0, 0, start);
    CHECK(text(model, warm) == "Warm");
    CHECK(model.parent(warm) == start);
    REQUIRE(model.rowCount(warm) == 1);
    const QModelIndex warmer = model.index(0, 0, warm);
    CHECK(text(model, warmer) == "Warmer");
    CHECK(model.parent(warmer) == warm);
    CHECK_FALSE(model.parent(start).isValid());
    CHECK(model.columnCount() == 1);

    CHECK(model.indexOfId(SnapshotId{4}) == warmer);
    CHECK_FALSE(model.indexOfId(SnapshotId{9}).isValid());
    CHECK(model.idOf(warm) == SnapshotId{3});
    CHECK_FALSE(model.idOf(QModelIndex{}));
}

TEST_CASE("Every role of a row", "[app][snapshots]") {
    SnapshotModel model;
    model.setSnapshots(forest(), SnapshotId{3}, SnapshotId{1});
    const QModelIndex start = model.indexOfId(SnapshotId{1});
    const QModelIndex warm = model.indexOfId(SnapshotId{3});

    CHECK(model.data(start, SnapshotModel::IdRole).toULongLong() == 1);
    CHECK(model.data(warm, SnapshotModel::IdRole).toULongLong() == 3);
    CHECK(model.data(start, SnapshotModel::ParentIdRole).toULongLong() == 0);
    CHECK(model.data(warm, SnapshotModel::ParentIdRole).toULongLong() == 1);
    CHECK(model.data(warm, SnapshotModel::CreatedRole).toDateTime() ==
          QDateTime::fromSecsSinceEpoch(1'790'000'000 + 180));

    CHECK(model.data(warm, SnapshotModel::BaseRole).toBool());
    CHECK_FALSE(model.data(start, SnapshotModel::BaseRole).toBool());
    CHECK(model.data(start, SnapshotModel::SavedBaseRole).toBool());
    CHECK_FALSE(model.data(warm, SnapshotModel::SavedBaseRole).toBool());

    CHECK(model.data(warm, Qt::FontRole).value<QFont>().bold());
    CHECK_FALSE(model.data(start, Qt::FontRole).isValid());

    const QString baseTip = text(model, warm, Qt::ToolTipRole);
    CHECK(baseTip.contains(model.data(warm, SnapshotModel::ChangeRole).toString()));
    CHECK_FALSE(baseTip.contains("Saved base"));
    CHECK(text(model, start, Qt::ToolTipRole).contains("Saved base"));

    CHECK(model.data(warm, Qt::DecorationRole).canConvert<QImage>());
}

TEST_CASE("The change of a row is worded as History words it", "[app][snapshots]") {
    SnapshotModel model;
    const SnapshotCollection collection = forest();
    model.setSnapshots(collection, std::nullopt, std::nullopt);

    CHECK(text(model, model.indexOfId(SnapshotId{1}), SnapshotModel::ChangeRole).isEmpty());
    // A root has no edge, whatever its recipe.
    CHECK(text(model, model.indexOfId(SnapshotId{2}), SnapshotModel::ChangeRole).isEmpty());

    const Snapshot& parent = *collection.find(SnapshotId{1});
    const Snapshot& child = *collection.find(SnapshotId{3});
    const QString expected =
        wordEdit(HistoryStep{parent.state, EditOrigin::Edit, {}, parent.negative},
                 HistoryStep{child.state, EditOrigin::Edit, {}, child.negative});
    CHECK(expected.startsWith("Exposure"));
    CHECK(text(model, model.indexOfId(SnapshotId{3}), SnapshotModel::ChangeRole) == expected);

    SECTION("an equal recipe says so") {
        SnapshotCollection same = collection;
        same.entries[2].state = same.entries[0].state;
        model.setSnapshots(same, std::nullopt, std::nullopt);
        CHECK(text(model, model.indexOfId(SnapshotId{3}), SnapshotModel::ChangeRole) ==
              "No changes");
    }
    SECTION("a delete reparents the children and their edges are worded again") {
        const QString before =
            text(model, model.indexOfId(SnapshotId{4}), SnapshotModel::ChangeRole);
        SnapshotCollection deleted = collection;
        deleted.entries.erase(deleted.entries.begin() + 2); // 3
        deleted.entries[2].parent = SnapshotId{1};          // 4 now below 1
        deleted.revision = 5;
        model.setSnapshots(deleted, std::nullopt, std::nullopt);

        const QModelIndex four = model.indexOfId(SnapshotId{4});
        CHECK(model.data(four, SnapshotModel::ParentIdRole).toULongLong() == 1);
        const Snapshot& moved = *deleted.find(SnapshotId{4});
        CHECK(text(model, four, SnapshotModel::ChangeRole) ==
              wordEdit(HistoryStep{parent.state, EditOrigin::Edit, {}, parent.negative},
                       HistoryStep{moved.state, EditOrigin::Edit, {}, moved.negative}));
        CHECK(text(model, four, SnapshotModel::ChangeRole) != before);
    }
}

TEST_CASE("A thumbnail or a rename changes rows; an identity or a parent resets",
          "[app][snapshots]") {
    SnapshotModel model;
    SnapshotCollection collection = forest();
    model.setSnapshots(collection, std::nullopt, std::nullopt);
    Watch watch(model);

    SECTION("a thumbnail") {
        collection.entries[0].thumbnail = thumbnailOf(Qt::red, 32, 24, "k1");
        model.setSnapshots(collection, std::nullopt, std::nullopt);
        CHECK(watch.resets == 0);
        CHECK(watch.changes >= 1);
    }
    SECTION("a rename") {
        collection.entries[1].name = "Renamed";
        collection.revision = 5;
        model.setSnapshots(collection, std::nullopt, std::nullopt);
        CHECK(watch.resets == 0);
        CHECK(watch.changes == 1);
        CHECK(text(model, model.indexOfId(SnapshotId{2})) == "Renamed");
    }
    SECTION("a change of base") {
        model.setSnapshots(collection, SnapshotId{2}, std::nullopt);
        CHECK(watch.resets == 0);
        CHECK(watch.changes == 1);
        CHECK(model.data(model.indexOfId(SnapshotId{2}), SnapshotModel::BaseRole).toBool());
    }
    SECTION("nothing at all") {
        model.setSnapshots(collection, std::nullopt, std::nullopt);
        CHECK(watch.resets == 0);
        CHECK(watch.changes == 0);
    }
    SECTION("an identity added") {
        collection.entries.push_back(snap(5, {}, "New", 0.1F));
        model.setSnapshots(collection, std::nullopt, std::nullopt);
        CHECK(watch.resets == 1);
        CHECK(model.rowCount() == 3);
    }
    SECTION("a parent changed") {
        collection.entries[3].parent = SnapshotId{1};
        model.setSnapshots(collection, std::nullopt, std::nullopt);
        CHECK(watch.resets == 1);
    }
    SECTION("cleared") {
        model.clear();
        CHECK(watch.resets == 1);
        CHECK(model.rowCount() == 0);
    }
}

TEST_CASE("A row shows a placeholder, then its thumbnail, then a fresh render",
          "[app][snapshots]") {
    SnapshotModel model;
    SnapshotCollection collection = forest();
    model.setSnapshots(collection, std::nullopt, std::nullopt);
    const QModelIndex first = model.indexOfId(SnapshotId{1});

    const auto picture = [&] { return model.data(first, Qt::DecorationRole).value<QImage>(); };
    const QImage placeholder = picture();
    REQUIRE_FALSE(placeholder.isNull());
    CHECK(placeholder.pixelColor(2, 2) == QColor(0x50, 0x50, 0x50));

    // A stored thumbnail is decoded and fitted to the side the rows show.
    collection.entries[0].thumbnail = thumbnailOf(Qt::red, 200, 100, "k1");
    model.setSnapshots(collection, std::nullopt, std::nullopt);
    QImage stored = picture();
    CHECK(stored.width() == SnapshotModel::pictureSide);
    CHECK(stored.height() == SnapshotModel::pictureSide / 2);
    CHECK(stored.pixelColor(10, 10).red() > 200);
    CHECK(stored.pixelColor(10, 10).green() < 60);

    // A render shown before its write has landed.
    QImage fresh(40, 40, QImage::Format_RGB32);
    fresh.fill(Qt::blue);
    model.setRendered(SnapshotId{1}, fresh);
    CHECK(picture().pixelColor(5, 5).blue() > 200);
    model.setRendered(SnapshotId{99}, fresh); // unknown: ignored

    // The stored one replaces it when the collection holds one with another key.
    collection.entries[0].thumbnail = thumbnailOf(Qt::green, 200, 100, "k2");
    model.setSnapshots(collection, std::nullopt, std::nullopt);
    CHECK(picture().pixelColor(10, 10).green() > 150);
}

TEST_CASE("A reset keeps the pictures already decoded", "[app][snapshots]") {
    SnapshotModel model;
    SnapshotCollection collection = forest();
    collection.entries[0].thumbnail = thumbnailOf(Qt::red, 32, 24, "k1");
    model.setSnapshots(collection, std::nullopt, std::nullopt);

    collection.entries.push_back(snap(5, {}, "New", 0.1F));
    model.setSnapshots(collection, std::nullopt, std::nullopt);
    const QImage kept =
        model.data(model.indexOfId(SnapshotId{1}), Qt::DecorationRole).value<QImage>();
    CHECK(kept.pixelColor(5, 5).red() > 200);
}
