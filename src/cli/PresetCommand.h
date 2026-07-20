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

// Dispatches `inv.verb` against the shared preset store (docs/adr/0051).
int runPreset(const PresetInvocation& inv, QTextStream& out, QTextStream& err);

} // namespace cli
