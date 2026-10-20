#pragma once

#include <filesystem>

namespace arraw {

/// Name of the environment variable that overrides the configuration directory.
inline constexpr const char* configDirectoryVariable = "ARRAW_CONFIG_DIR";

/// @brief Finds the directory where arraw keeps its shared configuration.
///
/// The same answer in the app, the command line and Python. In this order:
/// 1. ::arraw::configDirectoryVariable, when set and not empty, made absolute;
/// 2. `arraw` under the user's generic configuration location (Qt's
///    `QStandardPaths::GenericConfigLocation`): `~/.config/arraw` on Linux, which is where the
///    app's `QSettings` file lives, `~/Library/Preferences/arraw` on macOS and
///    `%LOCALAPPDATA%\arraw` on Windows.
///
/// The generic location is used, not the application's own, because the latter depends on the
/// application name the process sets, which the command line and Python do not.
/// Never creates the directory; the first write does.
/// @throws std::runtime_error if no directory can be found.
[[nodiscard]] std::filesystem::path configDirectory();

} // namespace arraw
