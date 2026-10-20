#pragma once

#include <Diagnostics.h>
#include <ExportPresets.h>

#include <QJsonObject>
#include <QJsonValue>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arraw {

/// @brief One entry of the presets file: what it said, and what arraw made of it.
struct PresetEntry {
    QJsonValue raw;                     ///< The entry as read, so that unknown keys survive.
    std::optional<ExportPreset> preset; ///< The decoded preset; absent when unusable.
    std::string name;                   ///< Its name as written (or as normalised, if usable).
    std::string reason;                 ///< Why it is unusable; empty when usable.
};

/// @brief The presets file, decoded but still holding everything it said.
struct ParsedPresets {
    std::vector<PresetEntry> entries; ///< Every entry, in file order.
    QJsonObject others;               ///< Top-level keys other than the version and the list.
    ExportPresetFileState state = ExportPresetFileState::Read; ///< What the file is.
    int version = exportPresetsVersion;                        ///< Version the file declares.
    std::string problem;                                       ///< Why it is unreadable.

    /// @brief Gives the usable and unusable entries as a catalog, in name order.
    [[nodiscard]] ExportPresetCatalog catalog() const;
};

/// Largest presets file that is read, in bytes.
inline constexpr std::size_t maxPresetsFileSize = 1024 * 1024;

/// @brief Reads the text of a presets file.
///
/// Never throws for the content: a file that cannot be read is reported with the state
/// Unreadable. Reports unknown keys, clamped and malformed values, unusable entries and a newer
/// version to the log.
/// @param text File contents.
/// @param log Where to report.
/// @param subject The file, for the log.
[[nodiscard]] ParsedPresets readExportPresets(std::string_view text, DiagnosticLog& log,
                                              const std::optional<std::filesystem::path>& subject);

/// @brief Writes the presets as the current version of the file.
///
/// Entries in name order (unusable ones by their name, nameless ones last); usable entries keep
/// the unknown keys they were read with; unusable ones are written as they were read.
/// @param parsed What to write; its state is ignored, the store decides whether to write.
[[nodiscard]] std::string writeExportPresets(const ParsedPresets& parsed);

/// @brief Gives the entry of a preset, keeping the unknown keys of the entry it replaces.
/// @param preset The preset to encode.
/// @param previous Entry it replaces; null for none.
[[nodiscard]] PresetEntry presetEntry(const ExportPreset& preset, const PresetEntry* previous);

/// @brief Folds a name for comparison: normalised, then case folded; empty if not valid.
[[nodiscard]] std::string foldedPresetName(std::string_view name);

} // namespace arraw
