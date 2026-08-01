#pragma once
#include <QStringList>
#include <QTextStream>

namespace cli {

// `arraw info <paths>...` (docs/adr/0053): reports each file's camera EXIF and
// sidecar-derived edit state, read-only. Never writes, never decodes pixels.
int runInfo(const QStringList& paths, bool json, QTextStream& out, QTextStream& err);

} // namespace cli
