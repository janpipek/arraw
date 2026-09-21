#pragma once

#include <iosfwd>
#include <string>
#include <vector>

/// @brief The `arraw-cli` command line, separated from its `main`.
///
/// `main` does nothing but construct the Qt application, collect `argv`, and
/// call ::arraw::cli::run. Everything else lives here so the tests can drive
/// the command line in-process and assert on what it writes to each stream;
/// see ADR 006.
namespace arraw::cli {

/// @brief Process exit codes, which scripts depend on.
///
/// Usage errors are distinguished from export failures because a script that
/// retries on failure would otherwise loop forever on a mistyped flag.
enum ExitCode {
    Success = 0,    ///< Every input was exported.
    Failed = 1,     ///< At least one input could not be exported.
    UsageError = 2, ///< The command line itself was wrong.
};

/// @brief Runs one invocation of the command line.
///
/// @param arguments Arguments *without* the program name.
/// @param out Text the caller asked for: help and version when requested
/// outright, and machine-readable output later. An export writes nothing here,
/// which is what keeps that channel free.
/// @param err Everything else: progress, warnings, errors, and the usage text
/// printed *at* someone who got the command line wrong.
/// @return One of ::ExitCode.
[[nodiscard]] int run(const std::vector<std::string>& arguments, std::ostream& out,
                      std::ostream& err);

} // namespace arraw::cli
