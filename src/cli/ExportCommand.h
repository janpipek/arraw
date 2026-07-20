#pragma once
#include "cli/ExportArgs.h"
#include <QTextStream>

namespace cli {

// Executes an export invocation end to end (docs/adr/0049): pre-flight, one
// HeadlessRenderContext, then each file sequentially through the exact GUI
// path — decode, sidecar resolution, corrected negative (DevelopSession),
// offscreen::renderToImage, runExportTail. Per-file errors are one stderr
// line and the run continues. Requires an existing QGuiApplication.
// Returns 0 (all exported), 1 (≥1 file failed), or 2 (environment error).
int runExport(const ExportInvocation& inv, QTextStream& out, QTextStream& err);

} // namespace cli
