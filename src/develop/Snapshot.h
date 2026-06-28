#pragma once

#include "develop/GlobalAdjustment.h"

#include <QString>

// A named, persisted capture of one photo's complete develop state, used to
// compare alternative development paths (A/B) by switching between them
// (docs/adr/0038). Unlike a Develop Preset, a Snapshot is whole-state — it
// always carries every field, including Local Adjustment masks and spots — and
// belongs to a single photo. Stored in the arraw: namespace of the develop
// sidecar (see XmpSidecar). Restoring one is an undoable History step; creating,
// renaming, and deleting snapshots edit the persisted list directly.
struct Snapshot {
    QString name;
    GlobalAdjustment state;
    bool operator==(const Snapshot&) const = default;
};
