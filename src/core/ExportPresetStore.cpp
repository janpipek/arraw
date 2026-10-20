#include "ExportPresetsJson.h"

#include <ConfigDirectory.h>
#include <ExportPresets.h>

#include <QDateTime>
#include <QLockFile>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>

namespace arraw {

namespace {

namespace fs = std::filesystem;

/// How long to wait for another process's change to finish, in milliseconds.
constexpr int lockWaitMs = 2000;

/// @brief Makes a QString of a path.
QString qpath(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// @brief Builds a built-in preset.
ExportPreset builtIn(std::string name, ExportSettings settings) {
    return {.name = std::move(name), .settings = settings, .builtIn = true};
}

/// @brief Tells whether a name is a built-in's.
bool isBuiltInName(std::string_view name) {
    return std::ranges::any_of(builtInExportPresets(), [&](const ExportPreset& preset) {
        return presetNamesEqual(preset.name, name);
    });
}

/// @brief Finds the first entry whose name folds like name.
PresetEntry* findEntry(ParsedPresets& parsed, std::string_view name) {
    const auto it = std::ranges::find_if(parsed.entries, [&](const PresetEntry& entry) {
        return presetNamesEqual(entry.name, name);
    });
    return it == parsed.entries.end() ? nullptr : &*it;
}

/// @brief Reads the file; Missing when there is none, Unreadable when it cannot be read.
ParsedPresets readFile(const fs::path& file, DiagnosticLog& log) {
    std::error_code error;
    if (!fs::exists(file, error)) {
        ParsedPresets missing;
        missing.state = ExportPresetFileState::Missing;
        missing.version = 0;
        return missing;
    }
    std::string text;
    std::ifstream in(file, std::ios::binary);
    if (in) {
        text.resize(maxPresetsFileSize + 1);
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(in.gcount()));
    }
    if (!in && !in.eof()) {
        ParsedPresets unreadable;
        unreadable.state = ExportPresetFileState::Unreadable;
        unreadable.problem = "the file cannot be opened or read";
        log.record({.notice = Notice::ExportPresetsUnreadable,
                    .severity = Severity::Error,
                    .subject = file,
                    .values = {unreadable.problem}});
        return unreadable;
    }
    return readExportPresets(text, log, file);
}

/// @brief Runs one change under the lock: re-read, refuse a file never to be written, apply,
/// replace the file.
void change(const fs::path& directory, const std::function<void(ParsedPresets&)>& apply) {
    const fs::path file = directory / exportPresetsFileName;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        throw ExportPresetError(ExportPresetProblem::WriteFailed,
                                "cannot create " + directory.string() + ": " + error.message());
    }
    QLockFile lock(qpath(file) + QStringLiteral(".lock"));
    if (!lock.tryLock(lockWaitMs)) {
        if (lock.error() == QLockFile::LockFailedError) {
            throw ExportPresetError(ExportPresetProblem::Busy, "another process is changing " +
                                                                   file.string() +
                                                                   "; try again in a moment");
        }
        throw ExportPresetError(ExportPresetProblem::WriteFailed,
                                "cannot lock " + file.string() + " for writing");
    }
    ParsedPresets parsed = readFile(file, discardedDiagnostics());
    if (parsed.state == ExportPresetFileState::Newer) {
        throw ExportPresetError(ExportPresetProblem::NewerFile,
                                file.string() + " was written by a newer arraw (version " +
                                    std::to_string(parsed.version) + "), so it is not changed");
    }
    if (parsed.state == ExportPresetFileState::Unreadable) {
        throw ExportPresetError(ExportPresetProblem::UnreadableFile,
                                file.string() + " cannot be read (" + parsed.problem +
                                    "), so it is not written over");
    }
    parsed.state = ExportPresetFileState::Read;
    apply(parsed);
    const std::string text = writeExportPresets(parsed);
    QSaveFile out(qpath(file));
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly) ||
        out.write(text.data(), static_cast<qint64>(text.size())) !=
            static_cast<qint64>(text.size()) ||
        !out.commit()) {
        throw ExportPresetError(ExportPresetProblem::WriteFailed,
                                "cannot write " + file.string() + ": " +
                                    out.errorString().toStdString());
    }
}

/// @brief Throws unless name is valid, and gives its normalised form.
std::string validName(std::string_view name) {
    if (const auto normalized = normalizedPresetName(name)) {
        return *normalized;
    }
    throw ExportPresetError(ExportPresetProblem::InvalidName,
                            "a preset name has 1 to 64 characters and no control characters");
}

/// @brief Throws if name is a built-in's.
void refuseBuiltIn(std::string_view name) {
    if (isBuiltInName(name)) {
        throw ExportPresetError(ExportPresetProblem::BuiltIn,
                                "'" + std::string(name) +
                                    "' is a built-in preset, which is read-only");
    }
}

} // namespace

std::span<const ExportPreset> builtInExportPresets() {
    static const std::array<ExportPreset, 4> presets{
        builtIn("Web 2048 px",
                {.resize = true, .width = 2048, .height = 2048, .quality = 85, .sharpening = 30}),
        builtIn("Social 1080 px",
                {.resize = true, .width = 1080, .height = 1080, .quality = 85, .sharpening = 30}),
        builtIn("Full size JPEG", {.quality = 92}),
        builtIn("Print TIFF 16-bit", {.format = ImageFileFormat::Tiff,
                                      .encoding = NamedEncoding::AdobeRgb,
                                      .sixteenBit = true}),
    };
    return presets;
}

ExportPresetStore::ExportPresetStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

ExportPresetStore ExportPresetStore::standard() {
    return ExportPresetStore(configDirectory());
}

std::filesystem::path ExportPresetStore::file() const {
    return directory_ / exportPresetsFileName;
}

ExportPresetCatalog ExportPresetStore::read(DiagnosticLog& log) const {
    ExportPresetCatalog fromFile = readFile(file(), log).catalog();
    ExportPresetCatalog catalog = fromFile;
    catalog.presets.assign(builtInExportPresets().begin(), builtInExportPresets().end());
    for (ExportPreset preset : fromFile.presets) {
        preset.shadowed = isBuiltInName(preset.name);
        catalog.presets.push_back(std::move(preset));
    }
    return catalog;
}

std::optional<ExportPreset> ExportPresetStore::load(std::string_view name,
                                                    DiagnosticLog& log) const {
    const ExportPresetCatalog catalog = read(log);
    for (const ExportPreset& preset : catalog.presets) {
        if (presetNamesEqual(preset.name, name)) {
            return preset;
        }
    }
    for (const UnusableExportPreset& preset : catalog.unusable) {
        if (presetNamesEqual(preset.name, name)) {
            throw ExportPresetError(ExportPresetProblem::Unusable,
                                    "the preset '" + preset.name + "' in " + file().string() +
                                        " cannot be used by this arraw: " + preset.reason);
        }
    }
    return std::nullopt;
}

void ExportPresetStore::save(const ExportPreset& preset) {
    const std::string name = validName(preset.name);
    refuseBuiltIn(name);
    validate(preset.settings);
    const ExportPreset clean{.name = name, .settings = preset.settings};
    change(directory_, [&](ParsedPresets& parsed) {
        if (PresetEntry* existing = findEntry(parsed, name)) {
            if (!existing->preset) {
                throw ExportPresetError(ExportPresetProblem::Unusable,
                                        "the preset '" + existing->name + "' in " +
                                            file().string() + " cannot be used by this arraw (" +
                                            existing->reason +
                                            "), so it is not replaced; remove it first");
            }
            *existing = presetEntry(clean, existing);
        } else {
            parsed.entries.push_back(presetEntry(clean, nullptr));
        }
    });
}

void ExportPresetStore::rename(std::string_view from, std::string_view to) {
    const std::string target = validName(to);
    refuseBuiltIn(target);
    change(directory_, [&](ParsedPresets& parsed) {
        PresetEntry* entry = findEntry(parsed, from);
        if (entry == nullptr) {
            refuseBuiltIn(from);
            throw ExportPresetError(ExportPresetProblem::NotFound, "there is no preset '" +
                                                                       std::string(from) + "' in " +
                                                                       file().string());
        }
        if (!entry->preset) {
            throw ExportPresetError(ExportPresetProblem::Unusable, "the preset '" + entry->name +
                                                                       "' cannot be used by this "
                                                                       "arraw (" +
                                                                       entry->reason +
                                                                       "), so it is not renamed");
        }
        for (const PresetEntry& other : parsed.entries) {
            if (&other != entry && presetNamesEqual(other.name, target)) {
                throw ExportPresetError(ExportPresetProblem::NameTaken,
                                        "a preset named '" + other.name + "' exists already");
            }
        }
        ExportPreset renamed = *entry->preset;
        renamed.name = target;
        *entry = presetEntry(renamed, entry);
    });
}

void ExportPresetStore::remove(std::string_view name) {
    change(directory_, [&](ParsedPresets& parsed) {
        PresetEntry* entry = findEntry(parsed, name);
        if (entry == nullptr) {
            refuseBuiltIn(name);
            throw ExportPresetError(ExportPresetProblem::NotFound, "there is no preset '" +
                                                                       std::string(name) + "' in " +
                                                                       file().string());
        }
        parsed.entries.erase(parsed.entries.begin() + (entry - parsed.entries.data()));
    });
}

std::filesystem::path ExportPresetStore::setAside() {
    const fs::path source = file();
    std::error_code error;
    if (!fs::exists(source, error)) {
        throw ExportPresetError(ExportPresetProblem::NotFound,
                                "there is no presets file at " + source.string());
    }
    QLockFile lock(qpath(source) + QStringLiteral(".lock"));
    if (!lock.tryLock(lockWaitMs)) {
        throw ExportPresetError(lock.error() == QLockFile::LockFailedError
                                    ? ExportPresetProblem::Busy
                                    : ExportPresetProblem::WriteFailed,
                                "cannot lock " + source.string() + " to set it aside");
    }
    const std::string stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")).toStdString();
    fs::path target;
    for (int n = 1;; ++n) {
        target = directory_ / ("export-presets.unreadable-" + stamp +
                               (n == 1 ? "" : "-" + std::to_string(n)) + ".json");
        if (!fs::exists(target, error)) {
            break;
        }
    }
    fs::rename(source, target, error);
    if (error) {
        throw ExportPresetError(ExportPresetProblem::WriteFailed,
                                "cannot move " + source.string() + ": " + error.message());
    }
    return target;
}

} // namespace arraw
