#pragma once

#include <Snapshots.h>

#include <QAbstractItemModel>
#include <QDateTime>
#include <QImage>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arraw::app {

/// @brief Snapshots of a photograph as a tree, a snapshot below the one it was captured from.
///
/// Holds a copy of the session's collection (at most ::arraw::maximumSnapshots nodes), fed by
/// ::arraw::app::SnapshotModel::setSnapshots after each change. It keeps no pointer to a session,
/// because every write invalidates the session's references. Roots and children are in
/// collection order, which is creation order. The model does not edit: a rename goes through the
/// snapshot writer, and the model shows the new name when the result is adopted.
class SnapshotModel : public QAbstractItemModel {
    Q_OBJECT

public:
    /// @brief What a row tells a view, beside Qt's own roles.
    enum Role {
        IdRole = Qt::UserRole + 1, ///< The snapshot's identity, as a `quint64`.
        ParentIdRole,              ///< The parent's identity, 0 for a root, as a `quint64`.
        CreatedRole,               ///< When it was captured, as a `QDateTime`.
        ChangeRole,                ///< The change from the parent, as a `QString`.
        BaseRole,                  ///< Whether it is the working base, as a `bool`.
        SavedBaseRole,             ///< Whether it is the base saved in the sidecar, as a `bool`.
    };

    /// @brief Longest side of the picture a row shows, in pixels.
    static constexpr int pictureSide = 96;

    /// @brief Makes an empty model.
    /// @param parent Owner.
    explicit SnapshotModel(QObject* parent = nullptr);

    /// @brief Shows a collection.
    ///
    /// The model resets when the order or the set of identities, or any parent, differs from what
    /// it shows; the caller then restores its selection by identity. Otherwise it emits
    /// `dataChanged` for each row that changed, so that a thumbnail or a rename keeps the
    /// selection.
    /// @param collection Snapshots, as ::arraw::EditSession::snapshots gives them.
    /// @param base Working base, shown in bold.
    /// @param savedBase Base the sidecar holds.
    void setSnapshots(const SnapshotCollection& collection, std::optional<SnapshotId> base,
                      std::optional<SnapshotId> savedBase);

    /// @brief Shows no snapshots.
    void clear();

    /// @brief Shows a freshly rendered picture of a snapshot before its write has landed.
    ///
    /// A picture stored with the snapshot replaces it when the collection holds one.
    /// @param id Snapshot the picture is of; an unknown one is ignored.
    /// @param image Picture; fitted to ::arraw::app::SnapshotModel::pictureSide.
    void setRendered(SnapshotId id, const QImage& image);

    /// @brief Gives the index of a snapshot.
    /// @param id Snapshot to look for.
    /// @return Its index, or an invalid one when the model shows none of that identity.
    [[nodiscard]] QModelIndex indexOfId(SnapshotId id) const;

    /// @brief Gives the snapshot of an index.
    /// @param index Index of the model.
    /// @return Its identity, or nothing for an invalid index.
    [[nodiscard]] std::optional<SnapshotId> idOf(const QModelIndex& index) const;

    /// @brief Gives the number of children of a snapshot, or of roots.
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;

    /// @brief Gives one column.
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;

    /// @brief Makes the index of a child.
    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent = {}) const override;

    /// @brief Gives the index of the parent snapshot.
    [[nodiscard]] QModelIndex parent(const QModelIndex& index) const override;

    /// @brief Gives the text or a role of a row.
    /// @param index Row.
    /// @param role A Qt role, or a ::arraw::app::SnapshotModel::Role.
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

private:
    /// @brief A snapshot and what the model works out from it.
    struct Node {
        Snapshot snapshot;
        /// Position of the parent in nodes_, or -1 for a root.
        int parent = -1;
        /// Positions of the children in nodes_, in collection order.
        std::vector<int> children;
        /// Row among the parent's children, or among the roots.
        int row = 0;
        /// Wording of the change from the parent.
        QString change;
        /// Picture, fitted; null until there is one.
        QImage picture;
        /// Render key of the stored thumbnail the picture was decoded from; empty if it is not.
        std::string pictureKey;
        /// Whether the picture came from ::arraw::app::SnapshotModel::setRendered.
        bool rendered = false;
        bool base = false;
        bool savedBase = false;
    };

    /// @brief Words the change of a node from its parent.
    [[nodiscard]] QString changeOf(const Node& node) const;

    /// @brief Takes the picture a node's thumbnail shows, decoding it if it is new.
    void takePicture(Node& node, const Snapshot& from);

    /// @brief Builds the tree structure over nodes_.
    void link();

    /// @brief Makes a node's internal id for an index.
    [[nodiscard]] const Node* nodeOf(const QModelIndex& index) const noexcept;

    std::vector<Node> nodes_;
    std::vector<int> roots_;
    /// Grey picture a row shows until it has a rendering.
    QImage placeholder_;
};

} // namespace arraw::app
