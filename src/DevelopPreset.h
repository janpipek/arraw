#pragma once

#include "DevelopGroup.h"

#include <QByteArray>
#include <QString>

// A saved, named bundle of selected Develop Groups (Milestone 8; docs/adr/0015).
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
