#pragma once

#include "Cli.h"

#include <Diagnostics.h>
#include <ExportPresets.h>

#include <QtCore/qcontainerfwd.h>

#include <iosfwd>
#include <optional>
#include <string_view>

class QCommandLineParser;

namespace arraw::cli {

/// @brief Lists and shows the export presets, read-only (ADR 054).
///
/// `arraw-cli export-preset list`, `export-preset show NAME` and `export-preset path`, each with
/// `--config-dir DIR`. `list` prints the names, one per line, the built-in ones marked
/// `(built-in)`; the entries of the file that cannot be used, and a file that cannot be read, are
/// reported on stderr. `show` prints the preset as the presets file holds it. `path` prints the
/// presets file's path, whether it exists or not. Presets are saved and deleted from Python and
/// the app, not here.
///
/// @param arguments The command's own arguments, beginning with its name.
/// @param out Help, and the listing.
/// @param err Errors and problems of the file.
/// @param start Starts the Qt application, a `QCoreApplication`, once the arguments are good.
/// @return ::arraw::cli::Success, or ::arraw::cli::UsageError if the arguments were wrong or the
/// preset asked for cannot be used.
[[nodiscard]] int runExportPresetCommand(const QStringList& arguments, std::ostream& out,
                                         std::ostream& err, const StartApplication& start);

/// @brief Adds `--config-dir` to a command's parser.
void addConfigDirOption(QCommandLineParser& parser);

/// @brief Opens the presets of `--config-dir`, or of the standard configuration directory.
/// @param parser Parser the option was added to and that has parsed.
/// @param command Word that selects the command, for the usage error.
/// @param err Stream a usage error is written to.
/// @param code Set to ::arraw::cli::UsageError when there is no configuration directory.
/// @return The store, or nothing after reporting the problem.
[[nodiscard]] std::optional<ExportPresetStore> openExportPresets(const QCommandLineParser& parser,
                                                                 std::string_view command,
                                                                 std::ostream& err, int& code);

/// @brief Finds a preset by name, reporting why when it cannot be used.
///
/// The built-in presets are found too. A file that cannot be read, an unknown name and a preset
/// this arraw cannot use are all usage errors that name the file and suggest `export-preset list`:
/// retrying will not fix them (ADR 006).
/// @param store Where to look.
/// @param name Name asked for, matched case-insensitively.
/// @param log Where the problems of the file are reported.
/// @param command Word that selects the command, for the usage error.
/// @param err Stream a usage error is written to.
/// @param code Set to ::arraw::cli::UsageError when the result is empty.
/// @return The preset, or nothing after reporting the problem.
[[nodiscard]] std::optional<ExportPreset>
findExportPreset(const ExportPresetStore& store, std::string_view name, DiagnosticLog& log,
                 std::string_view command, std::ostream& err, int& code);

} // namespace arraw::cli
