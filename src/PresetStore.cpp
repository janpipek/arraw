#include "PresetStore.h"

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
