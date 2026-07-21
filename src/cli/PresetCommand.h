#pragma once
#include "cli/PresetArgs.h"
#include "io/PresetStore.h"
#include <QTextStream>

namespace cli {

// Table by default, one JSON array with `json` (docs/adr/0050): each element
// is `{"name", "groups": [<developGroupKey>, ...]}`. An empty store is not an
// error. Always exits 0 — listing cannot fail short of I/O the store already
// treats as "no presets" (docs/adr/0051).
int runPresetList(const PresetStore& store, bool json, QTextStream& out);

// Table mirrors the GUI's Manage Presets details view (per-group changed
// fields, "(resets to defaults)" for an active group with none); `json`
// emits the preset's native serializeDevelopPreset bytes verbatim — the
// on-disk format is the API (docs/adr/0051). `name` matches
// case-insensitively; a miss lists the available names on stderr and
// returns 2 (docs/adr/0050's usage-error tier).
int runPresetShow(
    const PresetStore& store, const QString& name, bool json, QTextStream& out, QTextStream& err);

// Applies `name`'s groups to every path's XMP sidecar, byte-for-byte the
// GUI's batch apply: XmpSidecar::loadAdjustments -> applyGroups ->
// XmpSidecar::saveAdjustments, preserving ratings/labels/snapshots
// (docs/adr/0051). `paths` is pre-flighted as a whole first (exists, not a
// directory, supported image extension) — any bad path or unknown preset
// name refuses the entire run and returns 2, touching nothing. Otherwise
// per-file write failures don't stop the batch: 1 if any failed, 0 if clean
// (docs/adr/0050's exit tiers).
int runPresetApply(
    const PresetStore& store,
    const QString& name,
    const QStringList& paths,
    bool json,
    QTextStream& out,
    QTextStream& err);

// Dispatches `inv.verb` against the shared preset store (docs/adr/0051).
int runPreset(const PresetInvocation& inv, QTextStream& out, QTextStream& err);

} // namespace cli
