#include "cli/PresetArgs.h"
#include "cli/ArgParse.h"
#include <CLI11.hpp>

namespace cli {

PresetParse parsePresetArgs(const std::vector<std::string>& args) {
    PresetParse res;

    CLI::App app{"Inspect and apply Develop Presets", "arraw preset"};
    app.require_subcommand(1);

    bool jsonList = false;
    CLI::App* listCmd = app.add_subcommand("list", "List saved presets");
    listCmd->add_flag("--json", jsonList, "Emit a JSON array instead of a table");

    bool jsonShow = false;
    std::string showName;
    CLI::App* showCmd = app.add_subcommand("show", "Show a preset's settings");
    showCmd->add_option("name", showName, "Preset name")->required();
    showCmd->add_flag("--json", jsonShow, "Emit the preset's native JSON instead of a details view");

    bool jsonApply = false;
    std::string applyName;
    std::vector<std::string> applyPaths;
    CLI::App* applyCmd = app.add_subcommand("apply", "Apply a preset to files' XMP sidecars");
    applyCmd->add_option("name", applyName, "Preset name")->required();
    applyCmd->add_option("paths", applyPaths, "Files to update")->required();
    applyCmd->add_flag("--json", jsonApply, "Emit a JSON result document instead of progress lines");

    res.exitCode = parseArgs(app, args, res.message);
    if (res.exitCode >= 0)
        return res;

    if (listCmd->parsed()) {
        res.invocation.verb = PresetVerb::List;
        res.invocation.json = jsonList;
    } else if (showCmd->parsed()) {
        res.invocation.verb = PresetVerb::Show;
        res.invocation.json = jsonShow;
        res.invocation.name = QString::fromStdString(showName);
    } else if (applyCmd->parsed()) {
        res.invocation.verb = PresetVerb::Apply;
        res.invocation.json = jsonApply;
        res.invocation.name = QString::fromStdString(applyName);
        for (const std::string& p : applyPaths)
            res.invocation.paths << QString::fromStdString(p);
    }
    return res;
}

} // namespace cli
