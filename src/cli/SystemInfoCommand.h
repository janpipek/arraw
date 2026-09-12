#pragma once
#include "cli/TextStyle.h"
#include <QTextStream>

namespace cli {

// `arraw system-info` (docs/adr/0057): the CLI counterpart of Help > System
// Info — GPU backend/device, file locations, and versions, read-only. Always
// exits 0: there is no per-item failure mode here, unlike `info`'s per-file
// reports. If no headless GPU backend can be created (no GPU/display), the
// GPU fields read "(not yet available)" (same placeholder the dialog uses)
// and a note is written to `err`, but the command still succeeds.
int runSystemInfo(bool json, QTextStream& out, QTextStream& err, const TextStyle& style = {});

} // namespace cli
