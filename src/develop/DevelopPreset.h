#pragma once
#include "develop/GlobalAdjustment.h"

#include "develop/DevelopGroup.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// A saved, named bundle of selected Develop Groups (docs/adr/0023).
// Invariant: `values` carries the source's fields only for the groups in
// `groups`; every other group is left at its default. Serialisation is *partial*
// — only the active groups are written, so a group's presence in the JSON file
// *is* the flag that it should be applied on load.
struct DevelopPreset {
    QString name;
    GroupSelection groups;
    GlobalAdjustment values;

    bool operator==(const DevelopPreset&) const = default;
};

// Pure JSON (no disk): pretty-printed bytes <-> preset. parse returns a
// default-constructed preset with `ok` false on malformed input; unknown keys
// and missing groups are tolerated (forward/backward compatibility seam).
QByteArray serializeDevelopPreset(const DevelopPreset& preset);
DevelopPreset parseDevelopPreset(const QByteArray& json, bool* ok = nullptr);

// One group's fields from `values`, in the same native representation preset
// files use — shared with `arraw info --json`, which reports a photo's
// non-default groups with exactly the schema `preset show --json` already
// exposes rather than inventing a second one (docs/adr/0053).
QJsonObject groupToJson(DevelopGroup g, const GlobalAdjustment& values);
