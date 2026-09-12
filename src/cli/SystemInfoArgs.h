#pragma once
#include <string>
#include <vector>
#include <QString>

namespace cli {

struct SystemInfoInvocation {
    bool json = false;
};

// Outcome of parsing `arraw system-info` (docs/adr/0057). exitCode -1: proceed
// with `invocation`; 0: print `message` to stdout and exit (help); 2: print
// `message` to stderr and exit (usage error).
struct SystemInfoParse {
    int exitCode = -1;
    QString message;
    SystemInfoInvocation invocation;
};

SystemInfoParse parseSystemInfoArgs(const std::vector<std::string>& args);

} // namespace cli
