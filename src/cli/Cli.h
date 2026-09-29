#pragma once

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

/// @brief The `arraw-cli` command line, separated from its `main`.
///
/// `main` does nothing but collect `argv`, call ::arraw::cli::run, and construct
/// the Qt application a command asks for. Everything else lives here so the tests can drive
/// the command line in-process and assert on what it writes to each stream;
/// see ADR 006.
namespace arraw::cli {

/// @brief Process exit codes, which scripts depend on.
///
/// Usage errors are distinguished from export failures because a script that
/// retries on failure would otherwise loop forever on a mistyped flag.
enum ExitCode {
    Success = 0,    ///< Every input was exported, or the probe passed.
    Failed = 1,     ///< At least one input could not be exported, or the probe failed.
    UsageError = 2, ///< The command line itself was wrong.
};

/// @brief Kind of Qt application a command runs inside.
enum class ApplicationKind {
    Core, ///< Qt Core only: image codecs, and no platform plugin or graphics driver.
    Gui,  ///< A platform plugin too, which graphics devices are created through.
};

/// @brief Starts the Qt application a command needs.
///
/// A command calls it once its arguments are known to be good and before it
/// does any work, so help and usage errors never load a platform plugin: one
/// that cannot start aborts the process, and Qt offers no way to catch that.
/// `main` constructs the application, and the first request wins; the tests,
/// already inside one, only record what was asked.
using StartApplication = std::function<void(ApplicationKind)>;

/// @brief Runs one invocation of the command line.
///
/// @param arguments Arguments *without* the program name.
/// @param out Text the caller asked for: help and version when requested
/// outright, a probe's report, and machine-readable output later. An export
/// writes nothing here, which is what keeps that channel free.
/// @param err Everything else: progress, warnings, errors, and the usage text
/// printed *at* someone who got the command line wrong.
/// @param start Starts the Qt application, when a command gets as far as needing one.
/// @return One of ::ExitCode.
[[nodiscard]] int run(const std::vector<std::string>& arguments, std::ostream& out,
                      std::ostream& err, const StartApplication& start);

/// @brief Name of the environment variable that keeps `arraw-cli` off the GPU.
inline constexpr char disableGpuVariable[] = "ARRAW_DISABLE_GPU";

/// @brief Checks whether a value of ::arraw::cli::disableGpuVariable turns the GPU off.
///
/// Any non-empty value other than `0` does, so `1` and `yes` do, and so, for
/// want of guessing, does `false`; unset, empty and `0` leave the GPU on. Off,
/// `gpu-test` fails saying why, before asking for the `QGuiApplication` that
/// would load a platform plugin and touch a graphics driver.
///
/// @param value The variable's value, or `nullptr` if it is unset.
/// @return `true` if the GPU is to be left alone.
[[nodiscard]] bool disablesGpu(const char* value) noexcept;

/// @brief Checks whether this process's environment turns the GPU off.
/// @return ::arraw::cli::disablesGpu of ::arraw::cli::disableGpuVariable's value.
[[nodiscard]] bool gpuDisabled();

} // namespace arraw::cli
