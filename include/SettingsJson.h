#pragma once

#include <DevelopSettings.h>
#include <DevelopState.h>
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

/// @brief Writes a whole develop state as a JSON document.
///
/// The settings document of ::arraw::settingsToJson plus one key (ADR 044):
/// `"localAdjustments": {"version": 1, "nextId": 4, "masks": [...]}`, which is always written, an
/// empty `masks` included, so that a written state applied onto any base gives that state back.
/// Each mask is `{"id", "type", "name", "enabled", "opacity", "invert", "geometry", "deltas"}`;
/// only non-zero deltas are written. A linear mask's geometry is `{"from": [x, y], "to": [x, y]}`,
/// a radial one's `{"centre": [x, y], "radius": [rx, ry], "angle": a, "feather": f}`.
/// @param state State to write.
/// @return The document.
/// @throws std::invalid_argument if @p state fails ::arraw::validate.
[[nodiscard]] std::string stateToJson(const DevelopState& state);

/// @brief Writes a state's local adjustments as the JSON object `stateToJson` holds under
/// `localAdjustments`, on one line.
///
/// What `info --json` lists, so that its geometry is spelled as the state document spells it.
/// @param state State whose list to write.
/// @return The object, `{"version": 1, "nextId": n, "masks": [...]}`.
/// @throws std::invalid_argument if @p state fails ::arraw::validate.
[[nodiscard]] std::string localAdjustmentsToJson(const DevelopState& state);

/// @brief Applies a state document onto a base.
///
/// The settings by ::arraw::applySettingsJson, with its warnings and errors. When
/// `localAdjustments` is present it replaces the base's list and counter whole; absent, they stay.
/// Per-mask problems are warnings and drop or repair that mask (see
/// ::arraw::readLocalAdjustments): an unknown type, a malformed entry, degenerate geometry, a
/// duplicate id (the later one) and every mask past 16 are dropped
/// (::arraw::Notice::LocalAdjustmentDropped); an unknown field is ignored
/// (::arraw::Notice::LocalAdjustmentFieldIgnored); a number out of range is clamped
/// (::arraw::Notice::SettingClamped); a version newer than ::arraw::localAdjustmentsVersion is
/// read as far as it is understood (::arraw::Notice::NewerLocalAdjustmentsVersion). The counter
/// is the stored one raised above every id kept. A `localAdjustments` that is not an object, or
/// whose `masks` is not a list, is reported (::arraw::Notice::SettingMalformed) and leaves the
/// base's list as it was.
/// @param json The document.
/// @param base State the document is applied onto.
/// @param log Where the warnings go.
/// @return The state, valid whenever @p base was.
/// @throws std::invalid_argument as ::arraw::applySettingsJson.
[[nodiscard]] DevelopState applyStateJson(std::string_view json, DevelopState base,
                                          DiagnosticLog& log = discardedDiagnostics());

} // namespace arraw
