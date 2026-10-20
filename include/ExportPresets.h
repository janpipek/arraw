#pragma once

#include <Diagnostics.h>
#include <ExportSettings.h>

#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
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
    bool builtIn = false;    ///< Whether arraw defines it in code: read-only, never in the file.
    /// Whether it is a preset of the file whose name a built-in also has: listed, but the
    /// built-in is the one found by name. Set by ::arraw::ExportPresetStore::read.
    bool shadowed = false;

    /// @brief Compares the name as written, every setting and the built-in and shadowed flags.
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

/// @brief What reading the presets gave: the built-in presets first, then the file's in name order.
struct ExportPresetCatalog {
    std::vector<ExportPreset> presets; ///< Usable presets: the built-in ones, then the file's
                                       ///< (shadowed ones flagged) by folded name.
    std::vector<UnusableExportPreset> unusable; ///< Entries this arraw cannot use.
    ExportPresetFileState state = ExportPresetFileState::Missing; ///< What the file is.
    int version = 0;     ///< Version the file declares; 0 when there is no file.
    std::string problem; ///< Why the file is unreadable; empty otherwise.
};

/// @brief Lists the presets arraw defines itself, in the order they are shown.
///
/// Read-only: they are never written to the presets file, and cannot be saved over, renamed or
/// removed. Their names are plain constants for now.
[[nodiscard]] std::span<const ExportPreset> builtInExportPresets();

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

/// @brief Why a change to the presets did not happen.
enum class ExportPresetProblem {
    InvalidName,    ///< The name is empty, too long or holds control characters.
    NameTaken,      ///< Another preset already has the new name.
    NotFound,       ///< No preset has the name, or there is no file.
    Unusable,       ///< The preset is one this arraw cannot use, so it is not replaced or renamed.
    NewerFile,      ///< A newer arraw wrote the file, so it is never written.
    UnreadableFile, ///< The file cannot be read, so it is never written over.
    Busy,           ///< Another process is changing the file and did not finish in time.
    WriteFailed,    ///< The file or its directory could not be written; the old file stays.
    BuiltIn,        ///< The name is a built-in preset's, which cannot be changed.
    NotUnreadable,  ///< The file can be read, so it is not set aside: only an unreadable one is.
};

/// @brief A change to the presets that could not be made.
///
/// `what()` is a sentence that names the file.
class ExportPresetError : public std::runtime_error {
public:
    /// @brief Makes an error.
    /// @param problem Why the change did not happen.
    /// @param message A sentence for the user.
    ExportPresetError(ExportPresetProblem problem, const std::string& message)
        : std::runtime_error(message), problem_(problem) {}

    /// @brief Gives the cause, so that a caller can react to it.
    [[nodiscard]] ExportPresetProblem problem() const noexcept {
        return problem_;
    }

private:
    ExportPresetProblem problem_;
};

/// @brief The export presets file of one directory, read and changed safely.
///
/// Reading takes no lock and never creates anything. Each change (save, rename, remove,
/// setAside) takes a lock file, re-reads the file, applies the one change and replaces the
/// file atomically, so that several processes lose none of each other's changes (ADR 054).
/// A newer or unreadable file is never written. The built-in presets are never written either.
class ExportPresetStore {
public:
    /// @brief Makes a store for a configuration directory.
    /// @param directory Where the presets file lives; it need not exist.
    explicit ExportPresetStore(std::filesystem::path directory);

    /// @brief Makes the store of ::arraw::configDirectory.
    /// @throws std::runtime_error if there is no configuration directory.
    [[nodiscard]] static ExportPresetStore standard();

    /// @brief Gives the path of the presets file, which may not exist.
    [[nodiscard]] std::filesystem::path file() const;

    /// @brief Reads the catalog: built-in presets, then the file's.
    ///
    /// Never throws for the file's content: a file that cannot be read gives an empty file part
    /// and the state Unreadable. A preset of the file that has a built-in's name is listed with
    /// `shadowed` set.
    /// @param log Where to report problems of the file.
    [[nodiscard]] ExportPresetCatalog read(DiagnosticLog& log = discardedDiagnostics()) const;

    /// @brief Finds a preset by name, case-insensitively; a built-in wins over a shadowed one.
    /// @param name Name to look for.
    /// @param log Where to report problems of the file.
    /// @return The preset, or nothing when there is none.
    /// @throws ExportPresetError (Unusable) if the name is that of a preset this arraw cannot use.
    [[nodiscard]] std::optional<ExportPreset>
    load(std::string_view name, DiagnosticLog& log = discardedDiagnostics()) const;

    /// @brief Adds a preset, or replaces the one whose name folds alike, taking the new spelling.
    /// @throws ExportPresetError InvalidName, BuiltIn, Unusable, NewerFile, UnreadableFile, Busy
    /// or WriteFailed.
    /// @throws std::invalid_argument if the settings fail ::arraw::validate.
    void save(const ExportPreset& preset);

    /// @brief Gives a preset another name; a change of letter case only is allowed.
    /// @throws ExportPresetError NotFound, InvalidName, NameTaken, BuiltIn, Unusable, NewerFile,
    /// UnreadableFile, Busy or WriteFailed.
    void rename(std::string_view from, std::string_view to);

    /// @brief Removes a preset, usable or not.
    ///
    /// A built-in cannot be removed (BuiltIn), but a preset of the file that shadows one can.
    /// @throws ExportPresetError NotFound, BuiltIn, NewerFile, UnreadableFile, Busy or
    /// WriteFailed.
    void remove(std::string_view name);

    /// @brief Moves the file to `export-presets.unreadable-<YYYYMMDD-HHMMSS>.json` beside it.
    ///
    /// Never replaces a file; the next write starts a new one. Only a file that is unreadable
    /// when the lock is held is moved.
    /// @return The new path.
    /// @throws ExportPresetError NotFound (there is no file), NewerFile, NotUnreadable (the file
    /// reads fine), Busy or WriteFailed.
    std::filesystem::path setAside();

private:
    std::filesystem::path directory_;
};

} // namespace arraw
