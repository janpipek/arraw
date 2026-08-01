#include "cli/InfoArgs.h"
#include <CLI11.hpp>

namespace cli {

InfoParse parseInfoArgs(const std::vector<std::string>& args) {
    InfoParse res;

    CLI::App app{"Report EXIF and sidecar edit state for image files", "arraw info"};

    std::vector<std::string> paths;
    app.add_option("paths", paths, "Files to inspect")->required();
    bool json = false;
    app.add_flag("--json", json, "Emit a JSON array instead of per-file detail blocks");

    // CLI11's vector overload consumes back-to-front; hand it argc/argv.
    std::vector<const char*> argv;
    argv.push_back("arraw info");
    for (const std::string& a : args)
        argv.push_back(a.c_str());
    try {
        app.parse(int(argv.size()), argv.data());
    } catch (const CLI::CallForHelp&) {
        res.exitCode = 0;
        res.message = QString::fromStdString(app.help());
        return res;
    } catch (const CLI::ParseError& e) {
        res.exitCode = 2;
        res.message = QStringLiteral("arraw info: %1").arg(e.what());
        return res;
    }

    res.invocation.json = json;
    for (const std::string& p : paths)
        res.invocation.paths << QString::fromStdString(p);
    return res;
}

} // namespace cli
