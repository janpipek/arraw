#pragma once

#include <MarksFilter.h>

#include <QSortFilterProxyModel>

#include <functional>
#include <optional>

namespace arraw::app {

/// @brief Finds the row to move to when the active one is filtered out.
///
/// Main's rule: the nearest row at or after @p from that matches, else the
/// last row before it that does, so the view moves forward when it can.
/// Pure, so the rule is tested without a model.
/// @param rowCount Number of rows to look through, numbered from 0.
/// @param from Row to start from; one outside the range counts as the nearest end.
/// @param matches Tells whether a row matches the filter.
/// @return The row chosen, @p from itself when it matches, or nothing when no row does.
[[nodiscard]] std::optional<int> nearestMatchingRow(int rowCount, int from,
                                                    const std::function<bool(int)>& matches);

/// @brief Shows the shots of a ::arraw::app::ShotModel that pass a ::arraw::MarksFilter.
///
/// Filters dynamically: a change of marks in the source is judged again, so a
/// shot leaves the view the moment a rating takes it out of the filter, and
/// returns when it fits again. A shot whose sidecar has not been read yet is
/// shown, because its marks are not known and hiding it would make the view
/// flicker as the loader catches up.
class ShotFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    /// @brief Makes a proxy that lets everything through.
    /// @param parent Owner.
    explicit ShotFilterModel(QObject* parent = nullptr);

    /// @brief Sets what shots must have to be shown.
    /// @param filter Filter to apply.
    /// @throws std::invalid_argument if @p filter is incoherent (see
    /// ::arraw::MarksFilter::validate); the previous filter stays.
    void setFilter(const MarksFilter& filter);

    /// @brief Returns the filter applied.
    /// @return The filter; one that is not active shows everything.
    [[nodiscard]] const MarksFilter& filter() const noexcept {
        return filter_;
    }

    /// @brief Finds the shot to show when the one at a source row is not wanted.
    /// @param sourceRow Row of the source model to start from.
    /// @return Source row of the nearest shot that passes the filter (see
    /// ::arraw::app::nearestMatchingRow), or nothing when none does.
    [[nodiscard]] std::optional<int> nearestMatchingSourceRow(int sourceRow) const;

protected:
    /// @brief Tells whether a source row passes the filter.
    /// @param sourceRow Row of the source model.
    /// @param sourceParent Parent of the row; always the root.
    /// @return `true` if the shot is wanted.
    [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                        const QModelIndex& sourceParent) const override;

private:
    /// What shots must have to be shown.
    MarksFilter filter_;
};

} // namespace arraw::app
