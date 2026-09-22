#pragma once

#include <iosfwd>

#include <GeometrySettings.h>

#include <QtCore/qcontainerfwd.h>

namespace arraw::cli {

/// @brief Splits a clockwise angle into quarter-turn and straighten settings.
/// @param geometry Settings whose rotation and straighten values are replaced.
/// @param degrees Finite clockwise angle relative to camera orientation, before flips.
/// @throws std::invalid_argument if the angle is not finite.
void setRotationAngle(GeometrySettings& geometry, double degrees);

/// @brief Renders images and writes them out.
///
/// `arraw-cli export <input>... -o <dir> [options]`. Inputs are files rather
/// than directories, every input is attempted so one bad frame cannot abandon a
/// batch, and an existing output is refused unless replacing it was asked for.
/// See ADR 006.
///
/// @param arguments The command's own arguments, beginning with its name.
/// @param out Help, when it was asked for; nothing else.
/// @param err Progress, warnings, and errors.
/// @return ::arraw::cli::Success, ::arraw::cli::Failed if any input could not
/// be exported, or ::arraw::cli::UsageError if the arguments were wrong.
[[nodiscard]] int runExportCommand(const QStringList& arguments, std::ostream& out,
                                   std::ostream& err);

} // namespace arraw::cli
