#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QStringList>

// Backing model for the horizontal filmstrip. Holds the raw files of one
// directory in display order; the QListView + delegate are thin views over it.
class FilmStripModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { PathRole = Qt::UserRole + 1 };

    explicit FilmStripModel(QObject* parent = nullptr);

    void setFiles(QStringList paths);

    // Attaches a loaded thumbnail to its row, emitting dataChanged for it.
    // No-op if the path is not in the current file list.
    void setThumbnail(const QString& path, const QImage& thumb);

    // Model index of the row holding `path`, or an invalid index if absent.
    QModelIndex indexForPath(const QString& path) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    QStringList files;
    QHash<QString, QImage> thumbnails;  // keyed by path, survives reordering
};
