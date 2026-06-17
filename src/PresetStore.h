#pragma once

#include "DevelopPreset.h"

#include <vector>

#include <QString>

// Reads and writes Develop Presets as one JSON file per preset in a directory
// (Milestone 8; docs/adr/0015). The directory is injected so the store is
// testable headlessly; MainWindow constructs it with
// QStandardPaths::AppDataLocation/"presets". A preset's display name is the
// `name` inside the JSON — the filename is only storage, derived + sanitised.
class PresetStore {
public:
    explicit PresetStore(QString directory);

    // Writes <dir>/<presetFileName(name)>, creating the directory if needed.
    // Re-saving a preset with the same name overwrites it. Returns false on I/O
    // failure.
    bool save(const DevelopPreset& preset) const;

    // Every well-formed *.json in the directory, sorted case-insensitively by
    // display name. Malformed files are skipped, not fatal. Missing directory
    // yields an empty list.
    std::vector<DevelopPreset> loadAll() const;

    // Deletes the file backing `presetName`. Returns false if it was absent.
    bool remove(const QString& presetName) const;

private:
    QString directory;
};

// The storage filename for a preset display name: name with filesystem-unsafe
// characters replaced by '_', plus a ".json" suffix. Pure (no disk).
QString presetFileName(const QString& presetName);
