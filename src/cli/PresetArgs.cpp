#include "cli/PresetArgs.h"
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

    // CLI11's vector overload consumes back-to-front; hand it argc/argv.
    std::vector<const char*> argv;
    argv.push_back("arraw preset");
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
        res.message = QStringLiteral("arraw preset: %1").arg(e.what());
        return res;
    }

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
