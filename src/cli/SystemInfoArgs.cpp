#include "cli/SystemInfoArgs.h"
#include "cli/ArgParse.h"
#include <CLI11.hpp>

namespace cli {

SystemInfoParse parseSystemInfoArgs(const std::vector<std::string>& args) {
    SystemInfoParse res;

    CLI::App app{"Report GPU backend, file locations, and versions", "arraw system-info"};

    bool json = false;
    app.add_flag("--json", json, "Emit a JSON object instead of label:value rows");

    res.exitCode = parseArgs(app, args, res.message);
    if (res.exitCode >= 0)
        return res;

    res.invocation.json = json;
    return res;
}

} // namespace cli
