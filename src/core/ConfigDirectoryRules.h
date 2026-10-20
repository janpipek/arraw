#pragma once

#include <filesystem>
#include <string>

namespace arraw {

/// @brief Chooses the configuration directory from what the environment and the platform say.
///
/// A pure function, so that the rule is tested without touching the real environment.
/// @param override Value of ARRAW_CONFIG_DIR; empty when unset or empty.
/// @param genericConfig The platform's generic configuration location; empty when unknown.
/// @return The override made absolute, else `<genericConfig>/arraw`.
/// @throws std::runtime_error if both are empty, naming what is missing.
[[nodiscard]] std::filesystem::path resolveConfigDirectory(const std::u16string& override,
                                                           const std::u16string& genericConfig);

} // namespace arraw
