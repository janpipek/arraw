#include "ui/FilmStripFilterModel.h"
#include "develop/UserMetadata.h"
#include "ui/FilmStripModel.h"

FilmStripFilterModel::FilmStripFilterModel(QObject* parent)
    : QSortFilterProxyModel(parent) {}

void FilmStripFilterModel::setFilter(const FilmStripFilter& filter) {
    if (filter == activeFilter)
        return;
    activeFilter = filter;
    // invalidateFilter() is deprecated from 6.13 in favour of begin/endFilterChange,
    // which only exist from 6.9; keep building on the 6.8 CI toolchain.
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    endFilterChange();
#else
    invalidateFilter();
#endif
}

bool FilmStripFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    if (!activeFilter.isActive())
        return true; // fast path: an inactive filter is fully transparent
    const QAbstractItemModel* src = sourceModel();
    if (src == nullptr)
        return true;

    const QModelIndex idx = src->index(sourceRow, 0, sourceParent);
    UserMetadata m;
    m.rating = src->data(idx, FilmStripModel::RatingRole).toInt();
    m.label = ColourLabel(src->data(idx, FilmStripModel::LabelRole).toInt());
    return activeFilter.matches(m);
}
