#pragma once
#include "ui/FilmStripFilter.h"
#include <QSortFilterProxyModel>

/**
 * Hides film-strip rows that don't match the active rating/colour filter (ADR
 * 0042). A thin QSortFilterProxyModel over FilmStripModel: it reads RatingRole
 * and LabelRole from the source and defers the decision to FilmStripFilter.
 *
 * An inactive (default) filter accepts every row, so dropping this proxy under
 * the view is behaviour-neutral until a filter is set. Dynamic filtering stays
 * on, so a mark write on the source re-evaluates that row automatically.
 */
class FilmStripFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(FilmStripFilterModel)
public:
    explicit FilmStripFilterModel(QObject* parent = nullptr);

    // Replaces the active filter and re-evaluates all rows. No-op if unchanged.
    void setFilter(const FilmStripFilter& filter);

    const FilmStripFilter& filter() const { return activeFilter; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    FilmStripFilter activeFilter;
};
