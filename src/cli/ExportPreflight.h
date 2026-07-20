// Pre-flight of an export run (docs/adr/0049): derive each input's output
// path up front and refuse the whole run on unresolvable conditions — an
// input that is a directory (no folder mode in v1), or two inputs whose
// stems collide in the shared out-dir (failing at file 300 of 400 is the
// worst version; auto-suffixing invents names nobody asked for).

#pragma once

#include "ExportOptions.h"
#include <QString>
#include <QStringList>
#include <vector>

namespace cli {

struct ExportPlanItem {
    QString input;
    QString output;
};

struct ExportPlan {
    std::vector<ExportPlanItem> items;
    QString error; // empty = runnable
};

ExportPlan planExports(const QStringList& inputs, const QString& outDir, ExportOptions::Format format);

} // namespace cli
