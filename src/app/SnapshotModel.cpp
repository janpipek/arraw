#include "SnapshotModel.h"

#include "HistoryModel.h"

#include <EditSession.h>

#include <QColor>
#include <QCoreApplication>
#include <QFont>
#include <QLocale>
#include <QStringList>
#include <QVariant>

#include <algorithm>

namespace arraw::app {

namespace {

/// @brief Fits a picture to the side the rows show.
QImage fitted(const QImage& image) {
    if (image.isNull()) {
        return {};
    }
    const int side = SnapshotModel::pictureSide;
    if (image.width() <= side && image.height() <= side) {
        return image;
    }
    return image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

/// @brief Tells whether two collections list the same snapshots under the same parents, in the
/// same order.
template <typename Nodes> bool sameStructure(const Nodes& a, const std::vector<Snapshot>& b) {
    return std::ranges::equal(a, b, [](const auto& x, const Snapshot& y) {
        return x.snapshot.id == y.id && x.snapshot.parent == y.parent;
    });
}

} // namespace

SnapshotModel::SnapshotModel(QObject* parent)
    : QAbstractItemModel(parent),
      placeholder_(pictureSide, pictureSide * 3 / 4, QImage::Format_RGB32) {
    placeholder_.fill(QColor(0x50, 0x50, 0x50));
}

void SnapshotModel::link() {
    roots_.clear();
    for (Node& node : nodes_) {
        node.children.clear();
        node.parent = -1;
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        Node& node = nodes_[i];
        if (node.snapshot.parent) {
            const auto found = std::ranges::find_if(nodes_, [&](const Node& each) {
                return each.snapshot.id == *node.snapshot.parent;
            });
            if (found != nodes_.end()) {
                node.parent = static_cast<int>(found - nodes_.begin());
            }
        }
        if (node.parent < 0) {
            node.row = static_cast<int>(roots_.size());
            roots_.push_back(static_cast<int>(i));
        } else {
            auto& siblings = nodes_[static_cast<std::size_t>(node.parent)].children;
            node.row = static_cast<int>(siblings.size());
            siblings.push_back(static_cast<int>(i));
        }
    }
    for (Node& node : nodes_) {
        node.change = changeOf(node);
    }
}

QString SnapshotModel::changeOf(const Node& node) const {
    if (node.parent < 0) {
        return {};
    }
    const Snapshot& parent = nodes_[static_cast<std::size_t>(node.parent)].snapshot;
    if (parent.state == node.snapshot.state && parent.negative == node.snapshot.negative) {
        return QCoreApplication::translate("arraw::app::SnapshotModel", "No changes");
    }
    return wordEdit(HistoryStep{parent.state, EditOrigin::Edit, {}, parent.negative},
                    HistoryStep{node.snapshot.state, EditOrigin::Edit, {}, node.snapshot.negative});
}

void SnapshotModel::takePicture(Node& node, const Snapshot& from) {
    if (!from.thumbnail) {
        if (!node.rendered) {
            node.picture = {};
            node.pictureKey.clear();
        }
        return;
    }
    if (!node.rendered && node.pictureKey == from.thumbnail->renderKey && !node.picture.isNull()) {
        return;
    }
    QImage decoded;
    decoded.loadFromData(reinterpret_cast<const uchar*>(from.thumbnail->jpeg.data()),
                         static_cast<int>(from.thumbnail->jpeg.size()), "JPEG");
    if (decoded.isNull()) {
        // An undecodable stored picture looks like a missing one; a rendered one stays.
        if (!node.rendered) {
            node.picture = {};
            node.pictureKey.clear();
        }
        return;
    }
    node.picture = fitted(decoded);
    node.pictureKey = from.thumbnail->renderKey;
    node.rendered = false;
}

void SnapshotModel::setSnapshots(const SnapshotCollection& collection,
                                 std::optional<SnapshotId> base,
                                 std::optional<SnapshotId> savedBase) {
    if (!sameStructure(nodes_, collection.entries)) {
        beginResetModel();
        std::vector<Node> before = std::move(nodes_);
        nodes_.clear();
        nodes_.reserve(collection.entries.size());
        for (const Snapshot& snapshot : collection.entries) {
            Node node;
            node.snapshot = snapshot;
            // A picture already decoded for the same identity is kept, whatever moved.
            const auto kept = std::ranges::find_if(
                before, [&](const Node& each) { return each.snapshot.id == snapshot.id; });
            if (kept != before.end()) {
                node.picture = kept->picture;
                node.pictureKey = kept->pictureKey;
                node.rendered = kept->rendered;
            }
            takePicture(node, snapshot);
            node.base = base && *base == snapshot.id;
            node.savedBase = savedBase && *savedBase == snapshot.id;
            nodes_.push_back(std::move(node));
        }
        link();
        endResetModel();
        return;
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        Node& node = nodes_[i];
        const Snapshot& next = collection.entries[i];
        const bool isBase = base && *base == next.id;
        const bool isSaved = savedBase && *savedBase == next.id;
        const bool changedSnapshot = node.snapshot != next;
        if (!changedSnapshot && node.base == isBase && node.savedBase == isSaved) {
            continue;
        }
        const bool recipeChanged =
            node.snapshot.state != next.state || node.snapshot.negative != next.negative;
        node.snapshot = next;
        node.base = isBase;
        node.savedBase = isSaved;
        takePicture(node, next);
        if (recipeChanged) {
            node.change = changeOf(node);
        }
        const QModelIndex at = createIndex(node.row, 0, static_cast<quintptr>(i + 1));
        emit dataChanged(at, at);
        if (recipeChanged) {
            // The edges below this node are worded from it.
            for (const int child : node.children) {
                Node& below = nodes_[static_cast<std::size_t>(child)];
                below.change = changeOf(below);
                const QModelIndex belowAt =
                    createIndex(below.row, 0, static_cast<quintptr>(child + 1));
                emit dataChanged(belowAt, belowAt);
            }
        }
    }
}

void SnapshotModel::clear() {
    if (nodes_.empty()) {
        return;
    }
    beginResetModel();
    nodes_.clear();
    roots_.clear();
    endResetModel();
}

void SnapshotModel::setRendered(SnapshotId id, const QImage& image) {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        Node& node = nodes_[i];
        if (node.snapshot.id != id) {
            continue;
        }
        const QImage picture = fitted(image);
        if (picture.isNull()) {
            return;
        }
        node.picture = picture;
        node.pictureKey.clear();
        node.rendered = true;
        const QModelIndex at = createIndex(node.row, 0, static_cast<quintptr>(i + 1));
        emit dataChanged(at, at, {Qt::DecorationRole});
        return;
    }
}

QModelIndex SnapshotModel::indexOfId(SnapshotId id) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].snapshot.id == id) {
            return createIndex(nodes_[i].row, 0, static_cast<quintptr>(i + 1));
        }
    }
    return {};
}

std::optional<SnapshotId> SnapshotModel::idOf(const QModelIndex& index) const {
    const Node* node = nodeOf(index);
    if (node == nullptr) {
        return std::nullopt;
    }
    return node->snapshot.id;
}

const SnapshotModel::Node* SnapshotModel::nodeOf(const QModelIndex& index) const noexcept {
    if (!index.isValid() || index.model() != this) {
        return nullptr;
    }
    const auto at = static_cast<std::size_t>(index.internalId());
    if (at == 0 || at > nodes_.size()) {
        return nullptr;
    }
    return &nodes_[at - 1];
}

int SnapshotModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return static_cast<int>(roots_.size());
    }
    const Node* node = nodeOf(parent);
    return node == nullptr || parent.column() != 0 ? 0 : static_cast<int>(node->children.size());
}

int SnapshotModel::columnCount(const QModelIndex&) const {
    return 1;
}

QModelIndex SnapshotModel::index(int row, int column, const QModelIndex& parent) const {
    if (column != 0 || row < 0) {
        return {};
    }
    const std::vector<int>* siblings = &roots_;
    if (parent.isValid()) {
        const Node* node = nodeOf(parent);
        if (node == nullptr) {
            return {};
        }
        siblings = &node->children;
    }
    if (static_cast<std::size_t>(row) >= siblings->size()) {
        return {};
    }
    const int at = (*siblings)[static_cast<std::size_t>(row)];
    return createIndex(row, 0, static_cast<quintptr>(at + 1));
}

QModelIndex SnapshotModel::parent(const QModelIndex& index) const {
    const Node* node = nodeOf(index);
    if (node == nullptr || node->parent < 0) {
        return {};
    }
    const Node& up = nodes_[static_cast<std::size_t>(node->parent)];
    return createIndex(up.row, 0, static_cast<quintptr>(node->parent + 1));
}

QVariant SnapshotModel::data(const QModelIndex& index, int role) const {
    const Node* node = nodeOf(index);
    if (node == nullptr) {
        return {};
    }
    const Snapshot& snapshot = node->snapshot;
    switch (role) {
    case Qt::DisplayRole:
        return QString::fromStdString(snapshot.name);
    case Qt::DecorationRole:
        return node->picture.isNull() ? placeholder_ : node->picture;
    case Qt::FontRole: {
        if (!node->base) {
            return {};
        }
        QFont font;
        font.setBold(true);
        return font;
    }
    case Qt::ToolTipRole: {
        QStringList lines;
        lines << QLocale().toString(
            QDateTime::fromSecsSinceEpoch(snapshot.created.time_since_epoch().count()),
            QLocale::LongFormat);
        if (!node->change.isEmpty()) {
            lines << node->change;
        }
        if (node->savedBase) {
            lines << QCoreApplication::translate("arraw::app::SnapshotModel", "Saved base");
        }
        return lines.join(QLatin1Char('\n'));
    }
    case IdRole:
        return static_cast<quint64>(snapshot.id.value);
    case ParentIdRole:
        return static_cast<quint64>(snapshot.parent ? snapshot.parent->value : 0);
    case CreatedRole:
        return QDateTime::fromSecsSinceEpoch(snapshot.created.time_since_epoch().count());
    case ChangeRole:
        return node->change;
    case BaseRole:
        return node->base;
    case SavedBaseRole:
        return node->savedBase;
    default:
        return {};
    }
}

} // namespace arraw::app
