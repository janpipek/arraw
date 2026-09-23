#include "Cli.h"

#include "Command.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <iomanip>
#include <ostream>
#include <string_view>

namespace arraw::cli {
namespace {

/// @brief Column the command summaries line up in, in the top-level help.
constexpr int summaryColumn = 12;

/// @brief Writes the command table.
void listCommands(std::ostream& stream) {
    stream << "Commands:\n";
    for (const Command& command : commands()) {
        stream << "  " << std::left << std::setw(summaryColumn) << command.name << command.summary;
        if (command.run == nullptr) {
            stream << " (not implemented yet)";
        }
        stream << '\n';
    }
}

/// @brief Writes the help for the command line as a whole.
///
/// Only the frame is written here; the commands themselves come from the same
/// table dispatch uses, so this cannot advertise a command that does not exist.
/// A command's own options belong to its own parser, reached through
/// `arraw-cli <command> --help`.
void writeHelp(std::ostream& stream) {
    stream << "Develop and export photographs without opening the window.\n"
              "\n"
              "Usage: arraw-cli <command> [options]\n"
              "\n";
    listCommands(stream);
    stream << "\n"
              "Run 'arraw-cli <command> --help' for a command's own options.\n"
              "Set ARRAW_DISABLE_GPU=1 to run without loading any graphics stack.\n";
}

/// @brief Reports a usage problem, listing what could have been typed instead.
int usageError(std::ostream& err, const std::string& message) {
    err << "error: " << message << "\n\n";
    listCommands(err);
    err << "\nTry 'arraw-cli --help'.\n";
    return UsageError;
}

bool isHelpWord(const std::string& argument) {
    return argument == "--help" || argument == "-h" || argument == "help";
}

} // namespace
} // namespace arraw::cli

int arraw::cli::run(const std::vector<std::string>& arguments, std::ostream& out,
                    std::ostream& err) {
    if (arguments.empty()) {
        return usageError(err, "no command given");
    }

    const std::string& first = arguments.front();

    if (isHelpWord(first)) {
        // `arraw-cli help export` is the same question as `arraw-cli export
        // --help`, so it is answered by the command rather than duplicated here.
        if (arguments.size() > 1) {
            if (const Command* command = findCommand(arguments[1]); command != nullptr) {
                return run({arguments[1], "--help"}, out, err);
            }
            return usageError(err, "unknown command '" + arguments[1] + "'");
        }
        // Asked for outright, so it belongs on stdout where `| less` can reach
        // it. Usage printed *at* a mistake goes to stderr instead.
        writeHelp(out);
        return Success;
    }
    if (first == "--version" || first == "-v") {
        // The notice the GPL asks a program to be able to show, in the shape
        // GNU tools use. The authoritative statement is README.md and LICENSE;
        // this is so that a user holding only the binary can still find it.
        out << "arraw-cli " << ARRAW_VERSION << "\n"
            << "Copyright (C) 2026 Jan Pipek\n"
            << "License GPL-3.0-or-later: GNU GPL version 3 or later "
               "<https://gnu.org/licenses/gpl.html>.\n"
            << "This is free software: you are free to change and redistribute it.\n"
            << "There is NO WARRANTY, to the extent permitted by law.\n";
        return Success;
    }

    const Command* command = findCommand(first);
    if (command == nullptr) {
        return usageError(err, "unknown command '" + first + "'");
    }
    if (command->run == nullptr) {
        // Named in docs/desired-features.md and reserved here, but not built.
        // Distinguished from an unknown command so that someone who typed what
        // the documentation promised is told the feature is coming, not that
        // they mistyped.
        err << "error: '" << command->name << "' is not implemented yet\n";
        return UsageError;
    }

    // The command's own name leads, which is the element QCommandLineParser
    // expects to skip over.
    QStringList forwarded;
    forwarded.reserve(static_cast<qsizetype>(arguments.size()));
    for (const auto& argument : arguments) {
        forwarded << QString::fromStdString(argument);
    }
    return command->run(forwarded, out, err);
}

bool arraw::cli::disablesGpu(const char* value) noexcept {
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

bool arraw::cli::gpuDisabled() {
    // An unset variable reads as a null QByteArray, whose data is still "".
    return disablesGpu(qgetenv(disableGpuVariable).constData());
}
