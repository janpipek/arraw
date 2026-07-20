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

// Dispatches `inv.verb` against the shared preset store (docs/adr/0051).
int runPreset(const PresetInvocation& inv, QTextStream& out, QTextStream& err);

} // namespace cli
