#include "io/PresetStore.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QStandardPaths>

PresetStore defaultPresetStore() {
    return PresetStore(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/presets");
}

QString presetFileName(const QString& presetName) {
    QString safe;
    safe.reserve(presetName.size());
    for (const QChar c : presetName) {
        const bool ok = c.isLetterOrNumber() || c == ' ' || c == '-' || c == '_';
        safe.append(ok ? c : QChar('_'));
    }
    return safe + ".json";
}

PresetStore::PresetStore(QString directory)
    : directory(std::move(directory)) {}

bool PresetStore::save(const DevelopPreset& preset) const {
    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath("."))
        return false;

    QFile file(dir.filePath(presetFileName(preset.name)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(serializeDevelopPreset(preset));
    return true;
}

std::vector<DevelopPreset> PresetStore::loadAll() const {
    std::vector<DevelopPreset> presets;

    const QDir dir(directory);
    const QStringList files = dir.entryList({"*.json"}, QDir::Files);
    for (const QString& name : files) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        bool ok = false;
        DevelopPreset p = parseDevelopPreset(file.readAll(), &ok);
        if (ok)
            presets.push_back(std::move(p));
    }

    std::sort(presets.begin(), presets.end(), [](const DevelopPreset& a, const DevelopPreset& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return presets;
}

bool PresetStore::remove(const QString& presetName) const {
    return QFile::remove(QDir(directory).filePath(presetFileName(presetName)));
}

bool PresetStore::rename(const QString& oldName, const QString& newName) const {
    const QDir dir(directory);
    QFile oldFile(dir.filePath(presetFileName(oldName)));
    if (!oldFile.open(QIODevice::ReadOnly))
        return false;
    bool ok = false;
    DevelopPreset p = parseDevelopPreset(oldFile.readAll(), &ok);
    oldFile.close();
    if (!ok)
        return false;
    p.name = newName;

    // Write the new content to a temp path first — distinct from both oldName's
    // and newName's file, so it can never alias either — before touching the
    // old file. This way a write failure below never costs the original
    // preset, and a case-only rename (where oldName/newName alias the same
    // file on a case-insensitive filesystem) can't delete the just-written
    // content when the old file is cleared.
    const QString tempPath = dir.filePath(presetFileName(newName) + ".tmp");
    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    tempFile.write(serializeDevelopPreset(p));
    tempFile.close();

    QFile::remove(dir.filePath(presetFileName(oldName)));
    QFile::remove(dir.filePath(presetFileName(newName))); // clear any overwrite target
    return QFile::rename(tempPath, dir.filePath(presetFileName(newName)));
}

bool PresetStore::exists(const QString& presetName, const QString& excluding) const {
    const QString candidateFile = presetFileName(presetName);
    for (const DevelopPreset& p : loadAll()) {
        if (!excluding.isEmpty() && p.name == excluding)
            continue;
        if (p.name.compare(presetName, Qt::CaseInsensitive) == 0)
            return true;
        if (presetFileName(p.name).compare(candidateFile, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}
