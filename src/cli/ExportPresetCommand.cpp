#include "ExportPresetCommand.h"

#include "Command.h"
#include "StreamDiagnostics.h"
#include "TerminalStyle.h"

#include <Diagnostics.h>

#include <QCommandLineParser>
#include <QString>
#include <QStringList>

#include <exception>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

using namespace arraw;

namespace {

/// @brief Reports a usage problem of `export-preset`.
int usageError(std::ostream& err, const std::string& message) {
    return cli::commandUsageError(err, "export-preset", message);
}

/// @brief Declares the options and help of `export-preset`.
void configure(QCommandLineParser& parser) {
    parser.setApplicationDescription(
        "List and show the export presets, read-only.\n"
        "\n"
        "Presets are named export settings, kept in one file shared with the app and with\n"
        "Python, and used by `export --export-preset NAME`. Four built-in presets come first,\n"
        "marked (built-in); then the file's, by name. `list` prints one name per line; entries\n"
        "of the file this arraw cannot use, and a file it cannot read, are reported on stderr.\n"
        "`show NAME` prints a preset as the file holds it. `path` prints the file's path, which\n"
        "may not exist yet. Presets are saved and deleted from the app and from Python, not\n"
        "here. The file lives in the directory of --config-dir, else of ARRAW_CONFIG_DIR, else\n"
        "the user's arraw configuration directory.\n"
        "The exit status is 0 on success and 2 for a usage error, which includes a preset that\n"
        "does not exist or cannot be used.");
    parser.addHelpOption();
    cli::addConfigDirOption(parser);
    cli::addLogFormatOption(parser);
    parser.addPositionalArgument("action", "list, show or path.", "export-preset list|show|path");
    parser.addPositionalArgument("name", "Name of the preset to show.", "[name]");
}

/// @brief Writes the names of the presets, and reports the file's problems.
int list(const ExportPresetStore& store, cli::LogFormat format, std::ostream& out,
         std::ostream& err) {
    cli::StreamDiagnostics log(err, format);
    const ExportPresetCatalog catalog = store.read(log);
    for (const ExportPreset& preset : catalog.presets) {
        out << preset.name;
        if (preset.builtIn) {
            out << " (built-in)";
        } else if (preset.shadowed) {
            out << " (hidden by the built-in preset of the same name)";
        }
        out << "\n";
    }
    return cli::Success;
}

} // namespace

void cli::addConfigDirOption(QCommandLineParser& parser) {
    parser.addOption({"config-dir",
                      "Directory of the export presets file, instead of ARRAW_CONFIG_DIR or the "
                      "user's arraw configuration directory.",
                      "dir"});
}

std::optional<ExportPresetStore> cli::openExportPresets(const QCommandLineParser& parser,
                                                        std::string_view command, std::ostream& err,
                                                        int& code) {
    if (parser.isSet("config-dir")) {
        return ExportPresetStore(
            std::filesystem::path(parser.value("config-dir").toStdU16String()));
    }
    try {
        return ExportPresetStore::standard();
    } catch (const std::exception& problem) {
        code = commandUsageError(err, command, problem.what());
        return std::nullopt;
    }
}

std::optional<ExportPreset> cli::findExportPreset(const ExportPresetStore& store,
                                                  std::string_view name, DiagnosticLog& log,
                                                  std::string_view command, std::ostream& err,
                                                  int& code) {
    const std::string file = store.file().string();
    const std::string hint = "\n       `arraw-cli export-preset list` shows the presets";
    const ExportPresetCatalog catalog = store.read(log);
    // Built-in presets come first, so a preset of the file that shadows one is not the one found.
    for (const ExportPreset& preset : catalog.presets) {
        if (presetNamesEqual(preset.name, name)) {
            return preset;
        }
    }
    if (catalog.state == ExportPresetFileState::Unreadable) {
        code =
            commandUsageError(err, command,
                              "the presets file " + file + " cannot be read (" + catalog.problem +
                                  "), so '" + std::string(name) + "' is not found" + hint);
        return std::nullopt;
    }
    for (const UnusableExportPreset& preset : catalog.unusable) {
        if (presetNamesEqual(preset.name, name)) {
            code = commandUsageError(err, command,
                                     "the preset '" + preset.name + "' in " + file +
                                         " cannot be used by this arraw: " + preset.reason + hint);
            return std::nullopt;
        }
    }
    code = commandUsageError(err, command,
                             "no export preset is named '" + std::string(name) + "'" +
                                 (catalog.state == ExportPresetFileState::Missing
                                      ? " (there is no presets file, " + file + ")"
                                      : " in " + file) +
                                 hint);
    return std::nullopt;
}

int cli::runExportPresetCommand(const QStringList& arguments, std::ostream& out, std::ostream& err,
                                const StartApplication& start) {
    QCommandLineParser parser;
    configure(parser);
    if (!parser.parse(arguments)) {
        return usageError(err, parser.errorText().toStdString());
    }
    if (parser.isSet("help")) {
        writeStyledHelp(out, commandHelp(parser));
        return Success;
    }
    const QStringList positionals = parser.positionalArguments();
    if (positionals.isEmpty()) {
        return usageError(err, "give an action: list, show NAME or path");
    }
    const QString action = positionals.front();
    const bool shows = action == "show";
    if (!shows && action != "list" && action != "path") {
        return usageError(err, "unknown action '" + action.toStdString() +
                                   "'; expected list, show NAME or path");
    }
    if (positionals.size() != (shows ? 2 : 1)) {
        return usageError(err, shows ? "show takes one preset name"
                                     : action.toStdString() + " takes no arguments");
    }
    int code = Success;
    const auto format = readLogFormat(parser, "export-preset", err, code);
    if (!format) {
        return code;
    }
    const auto store = openExportPresets(parser, "export-preset", err, code);
    if (!store) {
        return code;
    }
    start(ApplicationKind::Core);
    if (action == "path") {
        out << store->file().string() << "\n";
        return Success;
    }
    if (action == "list") {
        return list(*store, *format, out, err);
    }
    StreamDiagnostics log(err, *format);
    const auto preset =
        findExportPreset(*store, positionals[1].toStdString(), log, "export-preset", err, code);
    if (!preset) {
        return code;
    }
    out << exportPresetToJson(*preset);
    return Success;
}
