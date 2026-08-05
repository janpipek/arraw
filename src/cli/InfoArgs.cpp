#include "cli/InfoArgs.h"
#include "cli/ArgParse.h"
#include <CLI11.hpp>

namespace cli {

InfoParse parseInfoArgs(const std::vector<std::string>& args) {
    InfoParse res;

    CLI::App app{"Report EXIF and sidecar edit state for image files", "arraw info"};

    std::vector<std::string> paths;
    app.add_option("paths", paths, "Files to inspect")->required();
    bool json = false;
    app.add_flag("--json", json, "Emit a JSON array instead of per-file detail blocks");

    res.exitCode = parseArgs(app, args, res.message);
    if (res.exitCode >= 0)
        return res;

    res.invocation.json = json;
    for (const std::string& p : paths)
        res.invocation.paths << QString::fromStdString(p);
    return res;
}

} // namespace cli
