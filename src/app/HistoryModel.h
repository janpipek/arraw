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

/// @brief Localised name of a settings group, as the develop panel titles it.
/// @param group Group to name.
/// @return The name; the Colour group also holds the white balance.
[[nodiscard]] QString groupDisplayName(SettingGroup group);

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
    void setHistory(const std::vector<HistoryStep>& history, std::size_t position,
                    const DevelopState& saved);

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
};

} // namespace arraw::app
