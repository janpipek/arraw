#include "ImageGrouping.h"
#include <QFileInfo>
#include <QHash>

namespace {

bool isRawExtension(const QString& suffix) {
    static const QStringList raw
        = {"cr2", "cr3", "nef", "arw", "dng", "raf", "orf", "rw2", "pef", "srw"};
    return raw.contains(suffix.toLower());
}

// The canonical, displayed name of a file's format. Folds the legacy/long
// spellings of a format onto one token (jpg/jpeg -> JPEG, tif/tiff -> TIFF);
// every other suffix (RAW types included) is its own uppercased extension.
QString canonicalFormatName(const QString& suffix) {
    const QString s = suffix.toLower();
    if (s == "jpg" || s == "jpeg")
        return "JPEG";
    if (s == "tif" || s == "tiff")
        return "TIFF";
    return suffix.toUpper();
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

QString formatLabelText(const ImageGroup& group) {
    QStringList tokens;
    const auto addFormat = [&tokens](const QString& path) {
        const QString name = canonicalFormatName(QFileInfo(path).suffix());
        if (!name.isEmpty() && !tokens.contains(name))
            tokens.append(name);
    };
    addFormat(group.primary);
    for (const QString& companion : group.companions)
        addFormat(companion);
    return tokens.join('+');
}
