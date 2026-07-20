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
    // `oldName` doesn't exist. Writes to a temp path before touching either the
    // old or new file, so a write failure never costs the original preset, and
    // a case-only rename is safe even where oldName/newName alias the same file
    // (a case-insensitive filesystem, e.g. Windows) — the temp path can't alias
    // either, so clearing the old file afterwards never risks the new content.
    bool rename(const QString& oldName, const QString& newName) const;

    // Whether a preset named `presetName` already exists — case-insensitively
    // (so every platform agrees) and post-sanitisation (so "a/b" and "a:b",
    // which presetFileName both map to "a_b.json", count as a collision even
    // though they are distinct display names). Used to prompt before an
    // overwrite on save/rename (CONTEXT.md Develop Preset, docs/adr/0049).
    // `excluding`, when given, skips the preset with that exact stored name —
    // so renaming a preset onto a case/sanitisation variant of its own current
    // name isn't reported as colliding with itself.
    bool exists(const QString& presetName, const QString& excluding = {}) const;

private:
    QString directory;
};

// The storage filename for a preset display name: name with filesystem-unsafe
// characters replaced by '_', plus a ".json" suffix. Pure (no disk).
QString presetFileName(const QString& presetName);
