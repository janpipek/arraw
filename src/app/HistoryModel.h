#pragma once

#include <DevelopState.h>
#include <EditSession.h>
#include <SettingDescriptors.h>

#include <QAbstractListModel>
#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

namespace arraw::app {

/// @brief Words an ordinary edit between two steps, as the History list does.
///
/// Shared with the snapshot tree, so that the two lists never word a change differently. The
/// wording is of the settings that differ; two equal steps give "Edit".
/// @param before Step the change starts from.
/// @param after Step the change leads to.
/// @return The wording.
[[nodiscard]] QString wordEdit(const HistoryStep& before, const HistoryStep& after);

/// @brief Steps of an edit session's history as a list, newest on top.
///
/// Holds a copy of the history (states are cheap to copy), fed by
/// ::arraw::app::HistoryModel::setHistory after each session change. Row 0 is the newest step and
/// the last row is "Opened".
class HistoryModel : public QAbstractListModel {
    Q_OBJECT

public:
    /// @brief What a row tells a view, beside Qt's own roles.
    enum Role {
        CurrentRole = Qt::UserRole + 1, ///< Whether the row is the current step, as a `bool`.
        RedoableRole,                   ///< Whether the row is above the current step, as a `bool`.
        SavedRole,                      ///< Whether the row's state is the saved one, as a `bool`.
    };

    /// @brief Makes an empty model.
    /// @param parent Owner.
    explicit HistoryModel(QObject* parent = nullptr);

    /// @brief Shows a session's history.
    /// @param history Steps, oldest first, as ::arraw::EditSession::history gives them.
    /// @param position Index of the current step.
    /// @param saved State on disk, to mark the steps that equal it.
    /// @param savedNegative Negative settings on disk, which a step must also equal.
    /// @param savedBase Working base on disk, which a step must also equal.
    void setHistory(const std::vector<HistoryStep>& history, std::size_t position,
                    const DevelopState& saved, const NegativeSettings& savedNegative,
                    std::optional<SnapshotId> savedBase);

    /// @brief Shows no steps.
    void clear();

    /// @brief Gives the number of steps.
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;

    /// @brief Gives the text or a role of a row.
    /// @param index Row.
    /// @param role `Qt::DisplayRole` (the wording) or a ::arraw::app::HistoryModel::Role.
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

    /// @brief Gives the row of a history index.
    /// @param index Index into the history, 0 for the oldest.
    [[nodiscard]] int rowOfIndex(std::size_t index) const noexcept;

    /// @brief Gives the history index of a row.
    /// @param row Row, 0 for the newest.
    [[nodiscard]] std::size_t indexOfRow(int row) const noexcept;

    /// @brief Gives the index of the current step in the history.
    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

private:
    /// @brief Words the step at a history index.
    [[nodiscard]] QString textOf(std::size_t index) const;

    std::vector<HistoryStep> history_;
    std::size_t position_ = 0;
    std::optional<DevelopState> saved_;
    NegativeSettings savedNegative_;
    std::optional<SnapshotId> savedBase_;
};

} // namespace arraw::app
