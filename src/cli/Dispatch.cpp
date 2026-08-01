#include "cli/Dispatch.h"
#include "cli/ExportArgs.h"
#include "cli/ExportCommand.h"
#include "cli/InfoArgs.h"
#include "cli/InfoCommand.h"
#include "cli/PresetArgs.h"
#include "cli/PresetCommand.h"
#include <QFileInfo>

namespace {

constexpr const char* kUsage = R"(usage: arraw [command] [options]

commands:
  ui [path]     open the editor on a file or folder (default with no command)
  export ...    render files through their develop sidecars; see 'arraw export --help'
  preset ...    list, show, or apply Develop Presets; see 'arraw preset --help'
  info <paths>  report EXIF and edit state for files; see 'arraw info --help'
  version       print the version
  help          show this help
)";

} // namespace

namespace cli {

int dispatch(int argc, char** argv, const GuiLauncher& launchUi, QTextStream& out, QTextStream& err) {
    if (argc < 2)
        return launchUi(QString()); // rule 1: bare invocation opens the UI

    const QString cmd = QString::fromLocal8Bit(argv[1]);

    if (cmd == QLatin1String("ui"))
        return launchUi(argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QString());

    if (cmd == QLatin1String("export")) {
        std::vector<std::string> args;
        for (int i = 2; i < argc; ++i)
            args.emplace_back(argv[i]);
        const ExportParse parsed = parseExportArgs(args);
        if (parsed.exitCode == 0) {
            out << parsed.message << "\n";
            return 0;
        }
        if (parsed.exitCode > 0) {
            err << parsed.message << "\n";
            return parsed.exitCode;
        }
        return runExport(parsed.invocation, out, err);
    }

    if (cmd == QLatin1String("preset")) {
        std::vector<std::string> args;
        for (int i = 2; i < argc; ++i)
            args.emplace_back(argv[i]);
        const PresetParse parsed = parsePresetArgs(args);
        if (parsed.exitCode == 0) {
            out << parsed.message << "\n";
            return 0;
        }
        if (parsed.exitCode > 0) {
            err << parsed.message << "\n";
            return parsed.exitCode;
        }
        return runPreset(parsed.invocation, out, err);
    }

    if (cmd == QLatin1String("info")) {
        std::vector<std::string> args;
        for (int i = 2; i < argc; ++i)
            args.emplace_back(argv[i]);
        const InfoParse parsed = parseInfoArgs(args);
        if (parsed.exitCode == 0) {
            out << parsed.message << "\n";
            return 0;
        }
        if (parsed.exitCode > 0) {
            err << parsed.message << "\n";
            return parsed.exitCode;
        }
        return runInfo(parsed.invocation.paths, parsed.invocation.json, out, err);
    }

    if (cmd == QLatin1String("version") || cmd == QLatin1String("--version")) {
        out << "arraw " << ARRAW_VERSION << "\n";
        return 0;
    }
    if (cmd == QLatin1String("help") || cmd == QLatin1String("--help")
        || cmd == QLatin1String("-h")) {
        out << kUsage;
        return 0;
    }

    err << "arraw: unknown command '" << cmd << "'\n"; // rule 3: loud, deterministic
    if (QFileInfo::exists(cmd))
        err << "to open it in the editor: arraw ui " << cmd << "\n";
    err << "run 'arraw help' for the command list\n";
    return 2;
}

} // namespace cli
