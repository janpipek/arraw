#include "ui/FilmStripModel.h"
#include "ui/FilmStripTooltip.h"
#include "ui/ImageGrouping.h"
#include <algorithm>
#include <QFileInfo>

namespace {

int compareNatural(const QString& left, const QString& right) {
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < left.size() && j < right.size()) {
        if (left.at(i).isDigit() && right.at(j).isDigit()) {
            const qsizetype leftStart = i;
            const qsizetype rightStart = j;
            while (i < left.size() && left.at(i).isDigit())
                ++i;
            while (j < right.size() && right.at(j).isDigit())
                ++j;

            qsizetype leftSig = leftStart;
            qsizetype rightSig = rightStart;
            while (leftSig < i && left.at(leftSig) == QLatin1Char('0'))
                ++leftSig;
            while (rightSig < j && right.at(rightSig) == QLatin1Char('0'))
                ++rightSig;

            const qsizetype leftLen = i - leftSig;
            const qsizetype rightLen = j - rightSig;
            if (leftLen != rightLen)
                return leftLen < rightLen ? -1 : 1;

            const int numberCompare = QStringView(left)
                                          .mid(leftSig, leftLen)
                                          .compare(QStringView(right).mid(rightSig, rightLen));
            if (numberCompare != 0)
                return numberCompare;
            if (i - leftStart != j - rightStart)
                return (i - leftStart) < (j - rightStart) ? -1 : 1;
            continue;
        }

        const QChar leftChar = left.at(i).toCaseFolded();
        const QChar rightChar = right.at(j).toCaseFolded();
        if (leftChar != rightChar)
            return leftChar < rightChar ? -1 : 1;
        ++i;
        ++j;
    }

    if (left.size() == right.size())
        return 0;
    return left.size() < right.size() ? -1 : 1;
}

} // namespace

FilmStripModel::FilmStripModel(QObject* parent)
    : QAbstractListModel(parent) {}

void FilmStripModel::setFiles(QStringList paths, QHash<QString, QStringList> companionsByPrimary) {
    std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) {
        return compareNatural(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
    });

    beginResetModel();
    files = std::move(paths);
    companions = std::move(companionsByPrimary);
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
    if (role == Qt::ToolTipRole) {
        const QString filename = QFileInfo(path).fileName();
        return metadata.contains(path) ? tooltipText(filename, metadata.value(path)) : filename;
    }
    if (role == PathRole)
        return path;
    if (role == Qt::DecorationRole)
        return thumbnails.value(path); // null QImage until loaded
    if (role == RatingRole)
        return marks.value(path).rating;
    if (role == LabelRole)
        return int(marks.value(path).label);
    if (role == CompanionsRole)
        return companions.value(path);
    if (role == FormatLabelRole)
        return formatLabelText({path, companions.value(path)});
    return {};
}

QModelIndex FilmStripModel::indexForPath(const QString& path) const {
    const int row = files.indexOf(path);
    return row < 0 ? QModelIndex{} : index(row);
}

void FilmStripModel::setThumbnail(const QString& path, const QImage& thumb) {
    const int row = files.indexOf(path);
    if (row < 0)
        return;
    thumbnails.insert(path, thumb);
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {Qt::DecorationRole});
}

void FilmStripModel::setMarks(const QString& path, const UserMetadata& m) {
    const int row = files.indexOf(path);
    if (row < 0)
        return;
    marks.insert(path, m);
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {RatingRole, LabelRole});
}

void FilmStripModel::setMetadata(const QString& path, const ImageMetadata& meta) {
    const int row = files.indexOf(path);
    if (row < 0)
        return;
    metadata.insert(path, meta);
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {Qt::ToolTipRole});
}
