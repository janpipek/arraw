#pragma once

#include <DevelopState.h>
#include <LocalAdjustments.h>

#include <QAbstractListModel>
#include <QString>

#include <optional>

namespace arraw::app {

/// @brief A state's masks as a list, in the order they sum in (ADR 044).
///
/// The model shows and never edits: an edit through a view (a rename, a check box) is emitted as
/// a request, and the window answers with a new state, as every other edit goes through the
/// session. Display is the name the mask is shown under (::arraw::app::maskDisplayName), edit its
/// own name (empty for an unnamed mask), check state its Enabled flag.
class MasksModel : public QAbstractListModel {
    Q_OBJECT

public:
    /// @brief Roles beyond Qt's.
    enum Role {
        IdRole = Qt::UserRole + 1, ///< The mask's id number (`uint`).
        KindRole,                  ///< 1 for a linear mask, 2 for a radial one (`int`).
        InvertedRole,              ///< Whether the mask is inverted (`bool`).
    };

    /// @brief Makes an empty model.
    explicit MasksModel(QObject* parent = nullptr);

    /// @brief Shows a state's masks.
    ///
    /// When the ids and their order are those shown, only the rows' data changes, so that a view
    /// keeps its selection and an open editor; otherwise the model resets.
    void setState(const DevelopState& state);

    /// @brief Gives the id of the mask in a row.
    [[nodiscard]] std::optional<LocalAdjustmentId> idAt(int row) const;

    /// @brief Gives the row of a mask, or -1 if it is not shown.
    [[nodiscard]] int rowOf(LocalAdjustmentId id) const;

    /// @brief Gives the number of masks.
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;

    /// @brief Gives a row's data.
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

    /// @brief Asks for a rename or an enable; changes nothing itself.
    ///
    /// Edit role: emits ::arraw::app::MasksModel::renameRequested. Check state role: emits
    /// ::arraw::app::MasksModel::enabledRequested.
    /// @return Whether the role is one the model takes a request for.
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    /// @brief Gives the item flags: selectable, checkable and editable.
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

signals:
    /// @brief A view asked for a mask to be renamed.
    /// @param id Mask to rename.
    /// @param name New name; empty for the default.
    void renameRequested(arraw::LocalAdjustmentId id, const QString& name);

    /// @brief A view asked for a mask to be enabled or disabled.
    /// @param id Mask to edit.
    /// @param enabled Whether it should act.
    void enabledRequested(arraw::LocalAdjustmentId id, bool enabled);

private:
    DevelopState state_;
};

} // namespace arraw::app
