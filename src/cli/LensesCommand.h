#pragma once

#include "Cli.h"

#include <QtCore/qcontainerfwd.h>

#include <iosfwd>

namespace arraw::cli {

/// @brief Lists the lenses of the lens database, read-only (ADR 050).
///
/// `arraw-cli lenses [--database DIR] [--camera TEXT] [--search TEXT] [--info]`. Lists the lenses
/// whose names hold every word of `--search`, and with `--camera` only those that fit the named
/// camera's mount; `--info` prints the database's directory, stamp and counts instead. It is the
/// tool for checking a database by hand and for the package smoke test. Without `--database`
/// the process's database is used (::arraw::LensDatabase::system).
///
/// @param arguments The command's own arguments, beginning with its name.
/// @param out Help, and the listing.
/// @param err Errors.
/// @param start Starts the Qt application, a `QCoreApplication`, once the arguments are good.
/// @return ::arraw::cli::Success, ::arraw::cli::Failed if no database could be loaded or the
/// camera is not in it, or ::arraw::cli::UsageError if the arguments were wrong.
[[nodiscard]] int runLensesCommand(const QStringList& arguments, std::ostream& out,
                                   std::ostream& err, const StartApplication& start);

} // namespace arraw::cli
