// The CLI11 call every `arraw <verb>` parser makes, in one place: build an
// App, hand it here, get back the ADR 0050 exit tier. Each parser used to
// carry its own copy of the argv marshalling and the two catch blocks, which
// is how the help text of one verb and the error prefix of another drift
// apart.

#pragma once

#include <string>
#include <vector>
#include <QString>

namespace CLI {
class App;
}

namespace cli {

// Parses `args` (the verb's own arguments, without argv[0]) with `app`.
// Returns -1 when the caller should go on and read `app`'s bound values; 0
// with `message` set to the help text for stdout; 2 with `message` set to a
// "<app name>: <what went wrong>" line for stderr.
int parseArgs(CLI::App& app, const std::vector<std::string>& args, QString& message);

} // namespace cli
