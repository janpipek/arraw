#include "FilmStripModel.h"
#include <QCollator>
#include <QFileInfo>
#include <algorithm>

FilmStripModel::FilmStripModel(QObject* parent) : QAbstractListModel(parent) {}

void FilmStripModel::setFiles(QStringList paths) {
    QCollator collator;
    collator.setNumericMode(true);                       // IMG_2 before IMG_10
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(paths.begin(), paths.end(),
              [&collator](const QString& a, const QString& b) {
                  return collator.compare(QFileInfo(a).fileName(),
                                          QFileInfo(b).fileName()) < 0;
              });

    beginResetModel();
    files = std::move(paths);
    endResetModel();
}

int FilmStripModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return files.size();
}

QVariant FilmStripModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= files.size())
        return {};
    const QString& path = files.at(index.row());
    if (role == Qt::DisplayRole)
        return QFileInfo(path).fileName();
    if (role == PathRole)
        return path;
    if (role == Qt::DecorationRole)
        return thumbnails.value(path);  // null QImage until loaded
    return {};
}

void FilmStripModel::setThumbnail(const QString& path, const QImage& thumb) {
    const int row = files.indexOf(path);
    if (row < 0)
        return;
    thumbnails.insert(path, thumb);
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {Qt::DecorationRole});
}
