#pragma once
#include "ExportOptions.h"
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

namespace cli {

struct ExportInvocation {
    QStringList inputs;
    QString outDir;
    ExportOptions options;
};

// Outcome of parsing `arraw export` arguments (docs/adr/0049). exitCode -1:
// proceed with `invocation`; 0: print `message` to stdout and exit (help);
// 2: print `message` to stderr and exit (usage error).
struct ExportParse {
    int exitCode = -1;
    QString message;
    ExportInvocation invocation;
};

ExportParse parseExportArgs(const std::vector<std::string>& args);

} // namespace cli
