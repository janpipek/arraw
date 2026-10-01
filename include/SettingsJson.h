#pragma once

#include <DevelopSettings.h>
#include <Diagnostics.h>

#include <string>
#include <string_view>

namespace arraw {

/// @brief Newest version of the settings document this arraw reads and writes.
inline constexpr int settingsJsonVersion = 1;

/// @brief Writes settings as a JSON document.
///
/// The document is `{"arraw": 1, "settings": {...}}`, indented for people, and
/// holds every key of the descriptor table in table order. An unset optional
/// is written as `null`.
/// @param settings Settings to write.
/// @return The document.
/// @throws std::invalid_argument if @p settings fail ::arraw::validate.
[[nodiscard]] std::string settingsToJson(const DevelopSettings& settings);

/// @brief Applies the settings in a JSON document onto a base.
///
/// A key that is absent leaves the base alone; `null` unsets an optional
/// setting. Per-setting problems are warnings and leave that setting as it was:
/// an unknown key (::arraw::Notice::SettingUnknown), a value of the wrong shape
/// (::arraw::Notice::SettingMalformed), a number out of range, which is
/// clamped (::arraw::Notice::SettingClamped). A document of a newer version is
/// still read, with ::arraw::Notice::NewerSettingsVersion.
/// @param json The document.
/// @param base Settings the document's keys are applied onto.
/// @param log Where the warnings go.
/// @return The settings, valid whenever @p base was.
/// @throws std::invalid_argument if the text is not JSON, is not an object with
/// an integer `"arraw"` version of at least 1 and an object `"settings"`, or if
/// the result fails ::arraw::validate. A number too large for a double (`1e999`)
/// is not readable JSON, so it rejects the whole document rather than one setting.
/// A key repeated within an object keeps its last value, without a warning.
[[nodiscard]] DevelopSettings applySettingsJson(std::string_view json, DevelopSettings base,
                                                DiagnosticLog& log = discardedDiagnostics());

} // namespace arraw
