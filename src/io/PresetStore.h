#pragma once

#include "develop/DevelopPreset.h"

#include <vector>

#include <QString>

// Reads and writes Develop Presets as one JSON file per preset in a directory
// (Milestone 8; docs/adr/0023). The directory is injected so the store is
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

    // Renames `oldName` to `newName`, keeping its groups and values, and
    // overwriting anything already stored under `newName`. Returns false if
    // `oldName` doesn't exist. The old file is removed before the new one is
    // written, so a case-only rename is safe on both case-sensitive (Linux) and
    // case-insensitive (Windows) filesystems — writing first then removing the
    // old path could otherwise delete the just-renamed file on Windows, where
    // the two paths alias the same file.
    bool rename(const QString& oldName, const QString& newName) const;

    // Whether a preset named `presetName` already exists — case-insensitively
    // (so every platform agrees) and post-sanitisation (so "a/b" and "a:b",
    // which presetFileName both map to "a_b.json", count as a collision even
    // though they are distinct display names). Used to prompt before an
    // overwrite on save/rename (CONTEXT.md Develop Preset, docs/adr/0049).
    bool exists(const QString& presetName) const;

private:
    QString directory;
};

// The storage filename for a preset display name: name with filesystem-unsafe
// characters replaced by '_', plus a ".json" suffix. Pure (no disk).
QString presetFileName(const QString& presetName);
