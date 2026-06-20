#include "ImageGrouping.h"
#include <QFileInfo>
#include <QHash>

namespace {

bool isRawExtension(const QString& suffix) {
    static const QStringList raw
        = {"cr2", "cr3", "nef", "arw", "dng", "raf", "orf", "rw2", "pef", "srw"};
    return raw.contains(suffix.toLower());
}

// Files of one capture: same directory, same case-insensitive base name.
QString captureKey(const QFileInfo& fi) {
    return fi.path() + "\n" + fi.completeBaseName().toLower();
}

struct Bucket {
    QStringList raws;
    QStringList standards;
};

} // namespace

QList<ImageGroup> groupImageFiles(const QStringList& paths) {
    QHash<QString, Bucket> buckets;
    for (const QString& path : paths) {
        const QFileInfo fi(path);
        Bucket& bucket = buckets[captureKey(fi)];
        if (isRawExtension(fi.suffix()))
            bucket.raws.append(path);
        else
            bucket.standards.append(path);
    }

    QList<ImageGroup> groups;
    for (const QString& path : paths) {
        const Bucket& bucket = buckets[captureKey(QFileInfo(path))];
        const bool pairable = bucket.raws.size() == 1;
        if (pairable && path == bucket.raws.front())
            groups.append({path, bucket.standards}); // RAW primary owns companions
        else if (!pairable)
            groups.append({path, {}}); // ambiguous or companion-less: stand alone
        // else: a standard companion already emitted with its RAW — skip
    }
    return groups;
}

QString companionBadgeText(const QStringList& companions) {
    if (companions.isEmpty())
        return {};
    QString badge = QFileInfo(companions.front()).suffix().toUpper();
    if (companions.size() > 1)
        badge += "+" + QString::number(companions.size() - 1);
    return badge;
}
