#include "cli/Dispatch.h"
#include "cli/ExportArgs.h"
#include "cli/ExportCommand.h"
#include "cli/InfoArgs.h"
#include "cli/InfoCommand.h"
#include "cli/PresetArgs.h"
#include "cli/PresetCommand.h"
#include <string>
#include <vector>
#include <QFileInfo>

namespace {

// The verb's own arguments: everything after `arraw <verb>`.
std::vector<std::string> verbArgs(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i)
        args.emplace_back(argv[i]);
    return args;
}

// The exit tier every verb shares (docs/adr/0050): help to stdout and 0, a
// usage error to stderr and its own code, otherwise run what was parsed.
template<typename Parse, typename Run>
int runVerb(const Parse& parsed, QTextStream& out, QTextStream& err, Run&& run) {
    if (parsed.exitCode == 0) {
        out << parsed.message << "\n";
        return 0;
    }
    if (parsed.exitCode > 0) {
        err << parsed.message << "\n";
        return parsed.exitCode;
    }
    return run(parsed.invocation);
}

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

int dispatch(
    int argc,
    char** argv,
    const GuiLauncher& launchUi,
    QTextStream& out,
    QTextStream& err,
    const TextStyle& style) {
    if (argc < 2)
        return launchUi(QString()); // rule 1: bare invocation opens the UI

    const QString cmd = QString::fromLocal8Bit(argv[1]);

    if (cmd == QLatin1String("ui"))
        return launchUi(argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QString());

    if (cmd == QLatin1String("export")) {
        return runVerb(
            parseExportArgs(verbArgs(argc, argv)), out, err, [&](const ExportInvocation& inv) {
                return runExport(inv, out, err);
            });
    }

    if (cmd == QLatin1String("preset")) {
        return runVerb(
            parsePresetArgs(verbArgs(argc, argv)), out, err, [&](const PresetInvocation& inv) {
                return runPreset(inv, out, err);
            });
    }

    if (cmd == QLatin1String("info")) {
        return runVerb(parseInfoArgs(verbArgs(argc, argv)), out, err, [&](const InfoInvocation& inv) {
            return runInfo(inv.paths, inv.json, out, err, style);
        });
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
