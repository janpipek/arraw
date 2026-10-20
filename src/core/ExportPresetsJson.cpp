#include "ExportPresetsJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace arraw {

namespace {

/// Greatest side of an export box, as validate accepts it.
constexpr double maxSide = 99999;

/// @brief Makes a QString of UTF-8 text.
QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// @brief Makes UTF-8 text of a QString.
std::string utf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

/// @brief Tells whether a code point is one a name may not hold.
bool isControl(char32_t c) {
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029;
}

/// @brief What decoding one entry gives, with what was wrong.
struct Decoded {
    ExportSettings settings;
    std::string unusable; ///< Reason the entry cannot be used; empty when it can.
};

/// @brief Decoding of one entry, which reports to the log with the entry's name as the prefix.
class EntryReader {
public:
    EntryReader(const QJsonObject& object, const std::string& name, DiagnosticLog& log,
                const std::optional<std::filesystem::path>& subject)
        : object_(object), name_(name), log_(log), subject_(subject) {}

    Decoded read() {
        Decoded result;
        ExportSettings& settings = result.settings;
        reportUnknown();
        if (const auto format = token("format"); format) {
            if (const auto parsed = parseImageFileFormat(*format)) {
                settings.format = *parsed;
            } else {
                return unusable("unknown format '" + *format + "'");
            }
        }
        if (const auto encoding = token("encoding"); encoding) {
            if (const auto parsed = parseOutputEncoding(*encoding)) {
                settings.encoding = *parsed;
            } else {
                return unusable("unknown encoding '" + *encoding + "'");
            }
        }
        if (const auto depth = number("bitDepth")) {
            if (*depth == 16) {
                settings.sixteenBit = true;
            } else if (*depth != 8) {
                return unusable("unsupported bit depth");
            }
        }
        if (object_.contains("size") && !object_.value("size").isNull()) {
            const QJsonValue size = object_.value("size");
            if (!size.isObject()) {
                return unusable("a size of an unknown shape");
            }
            const QJsonObject box = size.toObject();
            for (auto it = box.begin(); it != box.end(); ++it) {
                if (it.key() != "width" && it.key() != "height") {
                    unknown("size." + utf8(it.key()));
                }
            }
            if (!box.value("width").isDouble() || !box.value("height").isDouble()) {
                return unusable("a size of an unknown shape");
            }
            settings.resize = true;
            settings.width = static_cast<std::uint32_t>(
                clamped("size.width", box.value("width").toDouble(), 1, maxSide));
            settings.height = static_cast<std::uint32_t>(
                clamped("size.height", box.value("height").toDouble(), 1, maxSide));
        }
        if (const auto value = boolean(object_, "allowEnlarging", "allowEnlarging")) {
            settings.allowEnlarging = *value;
        }
        if (const auto value = number("quality")) {
            settings.quality = static_cast<int>(clamped("quality", *value, 0, 100));
        }
        if (const auto value = number("sharpening")) {
            settings.sharpening = static_cast<int>(clamped("sharpening", *value, 0, 100));
        }
        readMetadata(settings);
        settings = canonical(settings);
        return result;
    }

private:
    Decoded unusable(std::string reason) {
        Decoded result;
        result.unusable = std::move(reason);
        return result;
    }

    std::string key(const std::string& leaf) const {
        return name_ + "." + leaf;
    }

    void malformed(const std::string& leaf, const char* expected) {
        log_.record({.notice = Notice::SettingMalformed,
                     .severity = Severity::Warning,
                     .subject = subject_,
                     .values = {key(leaf), std::string(expected)}});
    }

    /// Reads a string, or nothing when absent or not a string (then reported).
    std::optional<std::string> token(const char* leaf) {
        if (!object_.contains(leaf)) {
            return std::nullopt;
        }
        const QJsonValue value = object_.value(leaf);
        if (!value.isString()) {
            malformed(leaf, "a word");
            return std::nullopt;
        }
        return utf8(value.toString());
    }

    /// Reads a number, or nothing when absent or not a number (then reported).
    std::optional<double> number(const char* leaf) {
        if (!object_.contains(leaf)) {
            return std::nullopt;
        }
        const QJsonValue value = object_.value(leaf);
        if (!value.isDouble() || !std::isfinite(value.toDouble())) {
            malformed(leaf, "a number");
            return std::nullopt;
        }
        return value.toDouble();
    }

    std::optional<bool> boolean(const QJsonObject& from, const char* leaf,
                                const std::string& path) {
        if (!from.contains(leaf)) {
            return std::nullopt;
        }
        const QJsonValue value = from.value(leaf);
        if (!value.isBool()) {
            malformed(path, "true or false");
            return std::nullopt;
        }
        return value.toBool();
    }

    double clamped(const std::string& leaf, double value, double low, double high) {
        const double limit = std::clamp(value, low, high);
        if (limit != value) {
            log_.record({.notice = Notice::SettingClamped,
                         .severity = Severity::Warning,
                         .subject = subject_,
                         .values = {key(leaf), value, limit}});
        }
        return std::round(limit);
    }

    void readMetadata(ExportSettings& settings) {
        if (!object_.contains("metadata")) {
            return;
        }
        const QJsonValue value = object_.value("metadata");
        if (!value.isObject()) {
            malformed("metadata", "an object");
            return;
        }
        const QJsonObject metadata = value.toObject();
        for (auto it = metadata.begin(); it != metadata.end(); ++it) {
            const std::string leaf = utf8(it.key());
            if (leaf != "capture" && leaf != "location" && leaf != "descriptive") {
                unknown("metadata." + leaf);
            }
        }
        if (const auto v = boolean(metadata, "capture", "metadata.capture")) {
            settings.captureInfo = *v;
        }
        if (const auto v = boolean(metadata, "location", "metadata.location")) {
            settings.location = *v;
        }
        if (const auto v = boolean(metadata, "descriptive", "metadata.descriptive")) {
            settings.descriptive = *v;
        }
    }

    void unknown(const std::string& leaf) {
        log_.record({.notice = Notice::SettingUnknown,
                     .severity = Severity::Warning,
                     .subject = subject_,
                     .values = {key(leaf)}});
    }

    void reportUnknown() {
        static const std::set<std::string> known{"name",     "format",     "encoding",
                                                 "bitDepth", "size",       "allowEnlarging",
                                                 "quality",  "sharpening", "metadata"};
        for (auto it = object_.begin(); it != object_.end(); ++it) {
            const std::string leaf = utf8(it.key());
            if (!known.contains(leaf)) {
                unknown(leaf);
            }
        }
    }

    const QJsonObject& object_;
    const std::string& name_;
    DiagnosticLog& log_;
    const std::optional<std::filesystem::path>& subject_;
};

/// @brief Makes the JSON object that holds a preset, laid over the keys of a previous one.
QJsonObject encode(const ExportPreset& preset, QJsonObject object) {
    const ExportSettings s = canonical(preset.settings);
    object["name"] = qs(preset.name);
    object["format"] = qs(tokenOf(s.format));
    object["encoding"] = qs(tokenOf(s.encoding));
    object["bitDepth"] = s.sixteenBit ? 16 : 8;
    if (s.resize) {
        QJsonObject box =
            object.value("size").isObject() ? object.value("size").toObject() : QJsonObject{};
        box["width"] = static_cast<double>(s.width);
        box["height"] = static_cast<double>(s.height);
        object["size"] = box;
    } else {
        object["size"] = QJsonValue::Null;
    }
    object["allowEnlarging"] = s.allowEnlarging;
    object["quality"] = s.quality;
    object["sharpening"] = s.sharpening;
    QJsonObject metadata =
        object.value("metadata").isObject() ? object.value("metadata").toObject() : QJsonObject{};
    metadata["capture"] = s.captureInfo;
    metadata["location"] = s.location;
    metadata["descriptive"] = s.descriptive;
    object["metadata"] = metadata;
    return object;
}

/// @brief Orders entries by folded name, then by name as written; nameless ones last.
bool entryBefore(const PresetEntry& a, const PresetEntry& b) {
    const std::string fa = foldedPresetName(a.name);
    const std::string fb = foldedPresetName(b.name);
    if (fa.empty() != fb.empty()) {
        return fb.empty();
    }
    return fa != fb ? fa < fb : a.name < b.name;
}

/// @brief Tells whether text holds only white space.
bool isBlank(std::string_view text) {
    return std::ranges::all_of(text, [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    });
}

} // namespace

std::optional<std::string> normalizedPresetName(std::string_view name) {
    const QString normalized = qs(name).trimmed().normalized(QString::NormalizationForm_C);
    const auto points = normalized.toUcs4();
    if (points.empty() || points.size() > 64) {
        return std::nullopt;
    }
    if (std::ranges::any_of(points, [](char32_t c) { return isControl(c); })) {
        return std::nullopt;
    }
    return utf8(normalized);
}

std::string foldedPresetName(std::string_view name) {
    const auto normalized = normalizedPresetName(name);
    return normalized ? utf8(qs(*normalized).toCaseFolded()) : std::string{};
}

bool presetNamesEqual(std::string_view a, std::string_view b) {
    const std::string folded = foldedPresetName(a);
    return !folded.empty() && folded == foldedPresetName(b);
}

PresetEntry presetEntry(const ExportPreset& preset, const PresetEntry* previous) {
    const auto name = normalizedPresetName(preset.name);
    if (!name) {
        throw std::invalid_argument("the preset name is not valid");
    }
    validate(preset.settings);
    ExportPreset clean{*name, canonical(preset.settings)};
    PresetEntry entry;
    QJsonObject base;
    if (previous != nullptr && previous->raw.isObject()) {
        base = previous->raw.toObject();
    }
    entry.raw = encode(clean, base);
    entry.name = *name;
    entry.preset = clean;
    return entry;
}

std::string exportPresetToJson(const ExportPreset& preset) {
    const PresetEntry entry = presetEntry(preset, nullptr);
    return utf8(
        QString::fromUtf8(QJsonDocument(entry.raw.toObject()).toJson(QJsonDocument::Indented)));
}

ExportPresetCatalog ParsedPresets::catalog() const {
    ExportPresetCatalog result;
    result.state = state;
    result.version = version;
    result.problem = problem;
    std::vector<const PresetEntry*> sorted;
    for (const PresetEntry& entry : entries) {
        sorted.push_back(&entry);
    }
    std::ranges::stable_sort(
        sorted, [](const PresetEntry* a, const PresetEntry* b) { return entryBefore(*a, *b); });
    for (const PresetEntry* entry : sorted) {
        if (entry->preset) {
            result.presets.push_back(*entry->preset);
        } else {
            result.unusable.push_back({entry->name, entry->reason});
        }
    }
    return result;
}

ParsedPresets readExportPresets(std::string_view text, DiagnosticLog& log,
                                const std::optional<std::filesystem::path>& subject) {
    ParsedPresets parsed;
    const auto unreadable = [&](std::string reason) {
        parsed.state = ExportPresetFileState::Unreadable;
        parsed.problem = std::move(reason);
        parsed.entries.clear();
        parsed.others = {};
        log.record({.notice = Notice::ExportPresetsUnreadable,
                    .severity = Severity::Error,
                    .subject = subject,
                    .values = {parsed.problem}});
        return parsed;
    };
    if (text.size() > maxPresetsFileSize) {
        return unreadable("the file is larger than 1 MiB");
    }
    if (isBlank(text)) {
        return parsed;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray(text.data(), static_cast<qsizetype>(text.size())), &error);
    if (error.error != QJsonParseError::NoError) {
        return unreadable("it is not JSON (" + utf8(error.errorString()) + ")");
    }
    if (!document.isObject()) {
        return unreadable("it is not a JSON object");
    }
    const QJsonObject root = document.object();
    const QJsonValue version = root.value("arraw");
    if (!version.isDouble() || version.toDouble() < 1 ||
        version.toDouble() != std::floor(version.toDouble()) || version.toDouble() > 1e9) {
        return unreadable("it has no integer \"arraw\" version of at least 1");
    }
    if (!root.value("exportPresets").isArray()) {
        return unreadable("it has no \"exportPresets\" list; it is not an export presets file");
    }
    parsed.version = static_cast<int>(version.toDouble());
    if (parsed.version > exportPresetsVersion) {
        parsed.state = ExportPresetFileState::Newer;
        log.record({.notice = Notice::NewerSettingsVersion,
                    .severity = Severity::Warning,
                    .subject = subject,
                    .values = {version.toDouble(), static_cast<double>(exportPresetsVersion)}});
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() != "arraw" && it.key() != "exportPresets") {
            parsed.others[it.key()] = it.value();
            log.record({.notice = Notice::SettingUnknown,
                        .severity = Severity::Warning,
                        .subject = subject,
                        .values = {utf8(it.key())}});
        }
    }

    std::set<std::string> taken;
    for (const QJsonValue entryValue : root.value("exportPresets").toArray()) {
        PresetEntry entry;
        entry.raw = entryValue;
        const auto fail = [&](std::string reason) {
            entry.preset.reset();
            entry.reason = std::move(reason);
            log.record({.notice = Notice::ExportPresetUnusable,
                        .severity = Severity::Warning,
                        .subject = subject,
                        .values = {entry.name, entry.reason}});
        };
        if (!entryValue.isObject()) {
            fail("it is not an object");
            parsed.entries.push_back(std::move(entry));
            continue;
        }
        const QJsonObject object = entryValue.toObject();
        if (object.value("name").isString()) {
            entry.name = utf8(object.value("name").toString());
        }
        const auto name =
            object.value("name").isString() ? normalizedPresetName(entry.name) : std::nullopt;
        if (!name) {
            fail("its name is missing or not valid");
            parsed.entries.push_back(std::move(entry));
            continue;
        }
        entry.name = *name;
        if (!taken.insert(foldedPresetName(*name)).second) {
            fail("duplicate name");
            parsed.entries.push_back(std::move(entry));
            continue;
        }
        const Decoded decoded = EntryReader(object, entry.name, log, subject).read();
        if (!decoded.unusable.empty()) {
            fail(decoded.unusable);
        } else {
            entry.preset = ExportPreset{entry.name, decoded.settings};
        }
        parsed.entries.push_back(std::move(entry));
    }
    return parsed;
}

std::string writeExportPresets(const ParsedPresets& parsed) {
    ParsedPresets sortedOrder = parsed;
    std::ranges::stable_sort(sortedOrder.entries, entryBefore);
    QJsonArray list;
    for (const PresetEntry& entry : sortedOrder.entries) {
        if (entry.preset) {
            list.append(encode(*entry.preset, entry.raw.toObject()));
        } else {
            list.append(entry.raw);
        }
    }
    QJsonObject root = parsed.others;
    root["arraw"] = exportPresetsVersion;
    root["exportPresets"] = list;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

} // namespace arraw
