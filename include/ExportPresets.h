#pragma once

#include "ExportSettings.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arraw {

/// Version of the export presets file this arraw writes (ADR 054).
inline constexpr int exportPresetsVersion = 1;

/// Name of the export presets file, in the configuration directory.
inline constexpr const char* exportPresetsFileName = "export-presets.json";

/// @brief A named, whole set of export settings.
struct ExportPreset {
    std::string name;        ///< Name shown to the user, normalised: see normalizedPresetName.
    ExportSettings settings; ///< Everything an export is asked for.

    /// @brief Compares the name as written and every setting.
    [[nodiscard]] bool operator==(const ExportPreset&) const = default;
};

/// @brief A preset in the file that this arraw cannot use.
///
/// Kept in the file as it is, so that a newer arraw's preset survives an older one's edit.
struct UnusableExportPreset {
    std::string name;   ///< Its name as written, empty when it has none.
    std::string reason; ///< Why it cannot be used, as a phrase.
};

/// @brief What reading the presets file found.
enum class ExportPresetFileState {
    Missing,    ///< There is no file: no presets, and nothing is created by reading.
    Read,       ///< The file was read, empty or not.
    Newer,      ///< A newer arraw wrote it: read as far as understood, never written.
    Unreadable, ///< It cannot be read: no presets, and it is never written over.
};

/// @brief The presets of a file, in name order.
struct ExportPresetCatalog {
    std::vector<ExportPreset> presets;          ///< Usable presets, sorted by folded name.
    std::vector<UnusableExportPreset> unusable; ///< Entries this arraw cannot use.
    ExportPresetFileState state = ExportPresetFileState::Missing; ///< What the file is.
    int version = 0;     ///< Version the file declares; 0 when there is no file.
    std::string problem; ///< Why the file is unreadable; empty otherwise.
};

/// @brief Gives the form of a preset name that is stored and compared.
///
/// Trimmed of white space at both ends and normalised to NFC. A valid name has 1 to 64
/// characters and no control characters (C0, DEL, C1, U+2028, U+2029).
/// @param name Name as typed or read.
/// @return The normalised name, or nothing when it is not valid.
[[nodiscard]] std::optional<std::string> normalizedPresetName(std::string_view name);

/// @brief Tells whether two names are the same preset: equal after normalising and case folding.
///
/// A name that is not valid is equal to nothing.
[[nodiscard]] bool presetNamesEqual(std::string_view a, std::string_view b);

/// @brief Writes a preset as a JSON object, as the presets file holds it.
///
/// Every key is written, indented, with a final line feed. Used by `export-preset show` and by
/// Python.
/// @param preset The preset; its settings are written in their canonical form.
/// @throws std::invalid_argument if the name or the settings are not valid.
[[nodiscard]] std::string exportPresetToJson(const ExportPreset& preset);

} // namespace arraw
