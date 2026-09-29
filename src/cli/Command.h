#pragma once

#include "Cli.h"

#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

#include <QtCore/qcontainerfwd.h>

class QCommandLineParser;

namespace arraw::cli {

/// @brief One word of the `arraw-cli` grammar.
///
/// Commands are plain rows in a table rather than a class hierarchy: each is a
/// free function with its own QCommandLineParser, so its `--help` lists its own
/// options and nothing else. Adding one is a row here and a file beside it.
struct Command {
    /// @brief Word that selects the command.
    std::string_view name;

    /// @brief One line describing it, for the top-level help.
    std::string_view summary;

    /// @brief Runs the command, or `nullptr` while the name is reserved but
    /// does nothing yet.
    ///
    /// @p arguments begins with the command's own name, which is what
    /// QCommandLineParser expects to skip over. @p start is called once the
    /// arguments are good, before any work; see ::arraw::cli::StartApplication.
    int (*run)(const QStringList& arguments, std::ostream& out, std::ostream& err,
               const StartApplication& start);
};

/// @brief Lists every command, in the order the help shows them.
///
/// The same table dispatch uses, so the help cannot advertise a command that
/// does not exist, nor hide one that does.
[[nodiscard]] std::span<const Command> commands();

/// @brief Finds a command by name.
/// @param name Word given on the command line.
/// @return The command, or `nullptr` if no command is called that.
[[nodiscard]] const Command* findCommand(std::string_view name);

/// @brief Writes a command's help, as its own parser generates it.
///
/// Qt names the program in the usage line after the application's first
/// argument, and help is answered before there is an application to ask, so
/// the name is set here: `arraw-cli`, as the top-level help calls it.
/// @param parser The command's configured parser.
/// @return The help text.
[[nodiscard]] std::string commandHelp(const QCommandLineParser& parser);

/// @brief Reports a usage problem with one command, pointing at its own help.
/// @param err Stream the problem is written to.
/// @param command Word that selects the command.
/// @param message What was wrong.
/// @return ::arraw::cli::UsageError.
int commandUsageError(std::ostream& err, std::string_view command, std::string_view message);

} // namespace arraw::cli
