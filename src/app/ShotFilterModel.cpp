#include "ShotFilterModel.h"

#include "ShotModel.h"

#include <QVariant>

#include <algorithm>
#include <stdexcept>

namespace arraw::app {

namespace {

/// @brief Turns the label role of a row into a label.
std::optional<ColorLabel> labelOf(int value) {
    const auto named = std::ranges::find_if(
        colorLabelNames, [&](const auto& entry) { return static_cast<int>(entry.first) == value; });
    if (named == colorLabelNames.end()) {
        return std::nullopt;
    }
    return named->first;
}

} // namespace

std::optional<int> nearestMatchingRow(int rowCount, int from,
                                      const std::function<bool(int)>& matches) {
    from = std::clamp(from, 0, std::max(rowCount, 1) - 1);
    for (int row = from; row < rowCount; ++row) {
        if (matches(row)) {
            return row;
        }
    }
    for (int row = std::min(from, rowCount) - 1; row >= 0; --row) {
        if (matches(row)) {
            return row;
        }
    }
    return std::nullopt;
}

ShotFilterModel::ShotFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
    // The proxy re-judges a row when a role it filters on changes.
    setFilterRole(ShotModel::RatingRole);
}

void ShotFilterModel::setFilter(const MarksFilter& filter) {
    filter.validate();
    if (filter == filter_) {
        return;
    }
    beginFilterChange();
    filter_ = filter;
    endFilterChange();
}

std::optional<int> ShotFilterModel::nearestMatchingSourceRow(int sourceRow) const {
    const QAbstractItemModel* source = sourceModel();
    if (source == nullptr) {
        return std::nullopt;
    }
    return nearestMatchingRow(source->rowCount(), sourceRow,
                              [&](int row) { return filterAcceptsRow(row, {}); });
}

bool ShotFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    const QAbstractItemModel* source = sourceModel();
    if (!filter_.isActive() || source == nullptr) {
        return true;
    }
    const QModelIndex index = source->index(sourceRow, 0, sourceParent);
    if (!source->data(index, ShotModel::MarksLoadedRole).toBool()) {
        return true;
    }
    const PhotoMarks marks{.rating = source->data(index, ShotModel::RatingRole).toInt(),
                           .label = labelOf(source->data(index, ShotModel::LabelRole).toInt())};
    return filter_.matches(marks);
}

} // namespace arraw::app
