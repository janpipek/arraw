#pragma once

#include "Cli.h"

#include <QtCore/qcontainerfwd.h>

#include <iosfwd>

namespace arraw::cli {

/// @brief Checks that the GPU backend works on this machine.
///
/// `arraw-cli gpu-test [--device <choice>] [--gpu-backend <name>] [--size <pixels>]
/// [--allow-software]`.
/// Creates offscreen devices through exactly one backend, reports what each is
/// and what it supports, then uploads an RGBA float test image and reads it
/// back. The round trip must be exact bit for bit, and the device a GPU: a
/// software rasteriser is refused unless accepting one was asked for, because a
/// probe that waved it through would be the fallback ADR 015 forbids. While
/// ::arraw::cli::disableGpuVariable turns the GPU off, it fails without trying.
///
/// `--device auto`, the default, tests every adapter the backend lists, each
/// in a block labelled `gpu0`, `gpu1` and so on, and continues past a failure.
/// A software adapter it was not told to accept is listed and skipped rather
/// than failed; the run fails only if nothing at all was tested. `gpu` tests the
/// backend's default device, `gpuN` adapter N alone, and `cpu` is a usage error.
/// A backend that lists no adapters has its default device tested, as `gpu0`.
///
/// @param arguments The command's own arguments, beginning with its name.
/// @param out The report, and help when it was asked for.
/// @param err Warnings and errors.
/// @param start Starts the Qt application, a `QGuiApplication`, once the
/// arguments are good and the GPU is not turned off.
/// @return ::arraw::cli::Success, ::arraw::cli::Failed if there is no usable
/// device, an adapter's round trip changed a sample, or none was tested, or
/// ::arraw::cli::UsageError if the arguments were wrong.
[[nodiscard]] int runGpuTestCommand(const QStringList& arguments, std::ostream& out,
                                    std::ostream& err, const StartApplication& start);

} // namespace arraw::cli
