#include "io/PresetStore.h"

#include <algorithm>

#include <QDir>
#include <QFile>

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

    if (!QFile::remove(dir.filePath(presetFileName(oldName))))
        return false;

    p.name = newName;
    return save(p);
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
