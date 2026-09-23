#pragma once

#include <iosfwd>

#include <QtCore/qcontainerfwd.h>

namespace arraw::cli {

/// @brief Checks that the GPU backend works on this machine.
///
/// `arraw-cli gpu-test [--backend <name>] [--size <pixels>] [--allow-software]`.
/// Creates an offscreen device through exactly one backend, reports what it is
/// and what it supports, then uploads an RGBA float test image and reads it
/// back. The round trip must be exact bit for bit, and the device a GPU: a
/// software rasteriser is refused unless accepting one was asked for, because a
/// probe that waved it through would be the fallback ADR 015 forbids. While
/// ::arraw::cli::disableGpuVariable turns the GPU off, it fails without trying.
///
/// @param arguments The command's own arguments, beginning with its name.
/// @param out The report, and help when it was asked for.
/// @param err Warnings and errors.
/// @return ::arraw::cli::Success, ::arraw::cli::Failed if there is no usable
/// device or the round trip changed a sample, or ::arraw::cli::UsageError if the
/// arguments were wrong.
[[nodiscard]] int runGpuTestCommand(const QStringList& arguments, std::ostream& out,
                                    std::ostream& err);

} // namespace arraw::cli
