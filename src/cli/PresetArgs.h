#pragma once
#include <string>
#include <vector>
#include <QString>
#include <QStringList>

namespace cli {

enum class PresetVerb { List, Show, Apply };

struct PresetInvocation {
    PresetVerb verb = PresetVerb::List;
    bool json = false;
    QString name;      // show, apply
    QStringList paths; // apply
};

// Outcome of parsing `arraw preset ...` (docs/adr/0051). exitCode -1: proceed
// with `invocation`; 0: print `message` to stdout and exit (help); 2: print
// `message` to stderr and exit (usage error).
struct PresetParse {
    int exitCode = -1;
    QString message;
    PresetInvocation invocation;
};

PresetParse parsePresetArgs(const std::vector<std::string>& args);

} // namespace cli
