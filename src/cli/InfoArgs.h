#pragma once
#include <string>
#include <vector>
#include <QString>
#include <QStringList>

namespace cli {

struct InfoInvocation {
    bool json = false;
    QStringList paths;
};

// Outcome of parsing `arraw info ...` (docs/adr/0053). exitCode -1: proceed
// with `invocation`; 0: print `message` to stdout and exit (help); 2: print
// `message` to stderr and exit (usage error).
struct InfoParse {
    int exitCode = -1;
    QString message;
    InfoInvocation invocation;
};

InfoParse parseInfoArgs(const std::vector<std::string>& args);

} // namespace cli
