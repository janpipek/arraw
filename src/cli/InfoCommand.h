#pragma once

#include "Cli.h"

#include <QtCore/qcontainerfwd.h>

#include <iosfwd>

namespace arraw::cli {

/// @brief Shows what is known about photographs, without changing anything.
///
/// `arraw-cli info <file>... [--all] [--json] [--no-sidecar]`. Each file is
/// opened as a document, which reads no pixels: its size, orientation and
/// encoding, the sidecar it was read through, its culling marks, and the
/// develop settings that differ from the defaults, spelled as the codec spells
/// them. Every input is attempted, so one bad file does not hide the rest. The
/// report, in text or as one JSON document, goes to @p out; diagnostics go to
/// @p err. See ADR 006.
///
/// @param arguments The command's own arguments, beginning with its name.
/// @param out Help, and the report.
/// @param err Warnings and errors.
/// @param start Starts the Qt application, a `QCoreApplication`, once the
/// arguments are good.
/// @return ::arraw::cli::Success, ::arraw::cli::Failed if any input could not
/// be shown, or ::arraw::cli::UsageError if the arguments were wrong.
[[nodiscard]] int runInfoCommand(const QStringList& arguments, std::ostream& out, std::ostream& err,
                                 const StartApplication& start);

} // namespace arraw::cli
