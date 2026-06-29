#pragma once
#include "core/ImageMetadata.h"
#include "develop/UserMetadata.h"
#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QStringList>

/**
 * Backing model for the horizontal filmstrip.
 *
 * FilmStripModel holds the raw files of one directory in display order plus
 * cached thumbnails, culling marks, and tooltip metadata keyed by path. It has
 * no sidecar I/O and no selection policy; FilmStrip performs those duties around
 * the standard Qt item-model contract.
 */
class FilmStripModel : public QAbstractListModel {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(FilmStripModel)
public:
    enum Roles {
        PathRole = Qt::UserRole + 1,
        RatingRole,      // int: 0 unrated, -1 reject, 1..5 stars
        LabelRole,       // int: a ColourLabel value
        CompanionsRole,  // QStringList: same-shot files attached to this primary (e.g. the JPEG)
        FormatLabelRole, // QString: the shot's Format Label, e.g. "ARW" or "ARW+JPEG"
    };

    explicit FilmStripModel(QObject* parent = nullptr);

    // Sets the rows to the given primary paths. `companions` maps a primary path
    // to the same-shot files grouped under it (see ImageGrouping); primaries
    // absent from the map have no companions.
    void setFiles(QStringList paths, QHash<QString, QStringList> companions = {});

    // Attaches a loaded thumbnail to its row, emitting dataChanged for it.
    // No-op if the path is not in the current file list.
    void setThumbnail(const QString& path, const QImage& thumb);

    // Attaches culling marks to a row, emitting dataChanged for it.
    // No-op if the path is not in the current file list.
    void setMarks(const QString& path, const UserMetadata& marks);

    // Attaches cached EXIF metadata to a row for tooltip display.
    // No-op if the path is not in the current file list.
    void setMetadata(const QString& path, const ImageMetadata& metadata);

    bool hasMetadata(const QString& path) const { return metadata.contains(path); }

    // The marks currently held for a path (defaults if none / unknown path).
    UserMetadata marksFor(const QString& path) const { return marks.value(path); }

    // Model index of the row holding `path`, or an invalid index if absent.
    QModelIndex indexForPath(const QString& path) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    QStringList files;
    QHash<QString, QStringList> companions; // keyed by primary path
    QHash<QString, QImage> thumbnails;      // keyed by path, survives reordering
    QHash<QString, UserMetadata> marks;     // keyed by path, survives reordering
    QHash<QString, ImageMetadata> metadata; // keyed by path, survives reordering
};
