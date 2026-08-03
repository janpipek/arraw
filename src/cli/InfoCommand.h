#pragma once
#include "cli/TextStyle.h"
#include <QStringList>
#include <QTextStream>

namespace cli {

// `arraw info <paths>...` (docs/adr/0053): reports each file's camera EXIF and
// sidecar-derived edit state, read-only. Never writes, never decodes pixels.
// `style` colours the detail blocks; --json is never coloured, and the default
// plain style is what every non-terminal sink gets.
int runInfo(
    const QStringList& paths,
    bool json,
    QTextStream& out,
    QTextStream& err,
    const TextStyle& style = {});

} // namespace cli
