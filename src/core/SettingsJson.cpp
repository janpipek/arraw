#include "SettingsJson.h"

#include "LocalAdjustmentCodec.h"
#include "SettingCodec.h"

#include <BrushStrokes.h>
#include <ShortestDecimal.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

using namespace arraw;

namespace {

/// @brief Spells a finite number as JSON, in the shortest text that reads back the same.
std::string number(double value) {
    return shortestText(value);
}

/// @brief Quotes a string as JSON.
std::string jsonString(std::string_view text) {
    std::string result = "\"";
    for (const char character : text) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                constexpr std::string_view digits = "0123456789abcdef";
                result += "\\u00";
                result += digits[static_cast<unsigned char>(character) >> 4];
                result += digits[static_cast<unsigned char>(character) & 0xF];
            } else {
                result += character;
            }
        }
    }
    return result + '"';
}

/// @brief Reads a JSON value as an encoded one, or nothing when it has no such form.
std::optional<Encoded> unspell(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Null:
        return Encoded{};
    case QJsonValue::Bool:
        return Encoded{value.toBool()};
    case QJsonValue::Double:
        return Encoded{value.toDouble()};
    case QJsonValue::String:
        return Encoded{value.toString().toStdString()};
    case QJsonValue::Object: {
        // Qt keeps the members of an object sorted by key, not as written.
        Compound compound;
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!it.value().isDouble()) {
                return std::nullopt;
            }
            compound.emplace_back(it.key().toStdString(), it.value().toDouble());
        }
        return Encoded{std::move(compound)};
    }
    case QJsonValue::Array: {
        PointList points;
        for (const QJsonValue& entry : value.toArray()) {
            const QJsonArray pair = entry.toArray();
            if (!entry.isArray() || pair.size() != 2 || !pair[0].isDouble() ||
                !pair[1].isDouble()) {
                return std::nullopt;
            }
            points.emplace_back(pair[0].toDouble(), pair[1].toDouble());
        }
        return Encoded{std::move(points)};
    }
    case QJsonValue::Undefined:
        break;
    }
    return std::nullopt;
}

} // namespace

std::string arraw::encodedToJson(const Encoded& encoded) {
    struct Speller {
        std::string operator()(std::monostate) const {
            return "null";
        }
        std::string operator()(bool flag) const {
            return flag ? "true" : "false";
        }
        std::string operator()(double value) const {
            return number(value);
        }
        std::string operator()(const std::string& text) const {
            return jsonString(text);
        }
        std::string operator()(const Compound& compound) const {
            // In the order the codec gives, which reads left to right.
            std::string text = "{";
            for (const auto& [key, value] : compound) {
                text += text.size() > 1 ? ", " : "";
                text += jsonString(key) + ": " + number(value);
            }
            return text + '}';
        }
        std::string operator()(const PointList& points) const {
            std::string text = "[";
            for (const auto& [x, y] : points) {
                text += text.size() > 1 ? ", " : "";
                text += "[" + number(x) + ", " + number(y) + "]";
            }
            return text + ']';
        }
    };
    return std::visit(Speller{}, encoded);
}

namespace {

/// @brief Writes the document up to and including the settings object, without the closing brace.
std::string settingsDocumentHead(const DevelopSettings& settings) {
    validate(settings);
    std::string text =
        "{\n  \"arraw\": " + std::to_string(settingsJsonVersion) + ",\n  \"settings\": {\n";
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        text += "    \"" + std::string(descriptor.key) +
                "\": " + encodedToJson(encode(descriptor, settings));
        text += &descriptor == &developSettingDescriptors.back() ? "\n" : ",\n";
    }
    return text + "  }";
}

/// @brief Spells a written value as JSON.
std::string jsonOf(const WrittenValue& value) {
    struct Speller {
        std::string operator()(bool flag) const {
            return flag ? "true" : "false";
        }
        std::string operator()(double number) const {
            return shortestText(number);
        }
        std::string operator()(const std::string& text) const {
            return jsonString(text);
        }
    };
    return std::visit(Speller{}, value);
}

/// @brief Finds a written field's value as JSON.
std::string fieldJson(const WrittenFields& fields, std::string_view key) {
    for (const auto& [name, value] : fields) {
        if (name == key) {
            return jsonOf(value);
        }
    }
    return "null";
}

/// @brief Spells a list of strokes as JSON: whole, or as a count and a hash.
std::string strokesJson(const StrokeList& strokes, StrokeDetail detail) {
    if (detail == StrokeDetail::Digest) {
        return std::format("{{\"count\": {}, \"hash\": \"{:016x}\"}}", strokes.size(),
                           strokes.contentHash());
    }
    std::string text = "[";
    for (const auto& stroke : strokes.strokes()) {
        text += text.size() > 1 ? ", " : "";
        text += "{\"radius\": " + shortestText(shortestDouble(stroke->radius)) +
                ", \"hardness\": " + shortestText(shortestDouble(stroke->hardness)) +
                ", \"flow\": " + shortestText(shortestDouble(stroke->flow)) +
                ", \"erase\": " + (stroke->erase ? "true" : "false") + ", \"points\": [";
        bool first = true;
        for (const SensorPoint& point : stroke->points) {
            text += first ? "[" : ", [";
            text += shortestText(shortestDouble(point.u)) + ", " +
                    shortestText(shortestDouble(point.v)) + "]";
            first = false;
        }
        text += "]}";
    }
    return text + "]";
}

/// @brief Spells one mask as a one-line JSON object.
std::string maskToJson(const LocalAdjustment& adjustment, StrokeDetail detail) {
    const WrittenMask written = writtenForm(adjustment);
    std::string text = "{";
    for (const auto& [key, value] : written.top) {
        text += (text.size() > 1 ? ", " : "") + jsonString(key) + ": " + jsonOf(value);
    }
    const auto pair = [&](std::string_view name, std::string_view key) {
        return jsonString(name) + ": [" + fieldJson(written.geometry, std::string(key) + "X") +
               ", " + fieldJson(written.geometry, std::string(key) + "Y") + "]";
    };
    text += ", \"geometry\": {";
    if (std::holds_alternative<LinearMask>(adjustment.shape)) {
        text += pair("from", "from") + ", " + pair("to", "to");
    } else if (written.strokes) {
        text += "\"rasteriser\": " + fieldJson(written.geometry, "rasteriser") +
                ", \"strokes\": " + strokesJson(*written.strokes, detail);
    } else {
        text += pair("centre", "centre") + ", " + pair("radius", "radius") +
                ", \"angle\": " + fieldJson(written.geometry, "angle") +
                ", \"feather\": " + fieldJson(written.geometry, "feather");
    }
    text += "}, \"deltas\": {";
    bool first = true;
    for (const auto& [key, value] : written.deltas) {
        text += (first ? "" : ", ") + jsonString(key) + ": " + jsonOf(value);
        first = false;
    }
    return text + "}}";
}

/// @brief Spells the list object; one mask a line, indented, or all on one line.
std::string listJson(const DevelopState& state, bool lines, StrokeDetail detail) {
    std::string text = "{\"version\": " + std::to_string(localAdjustmentsVersion) +
                       ", \"nextId\": " + std::to_string(state.nextLocalAdjustmentId.value) +
                       ", \"masks\": [";
    for (std::size_t i = 0; i < state.localAdjustments.size(); ++i) {
        text += lines ? "\n    " : (i > 0 ? ", " : "");
        text += maskToJson(state.localAdjustments[i], detail);
        text += lines && i + 1 < state.localAdjustments.size() ? "," : "";
    }
    if (lines && !state.localAdjustments.empty()) {
        text += "\n  ";
    }
    return text + "]}";
}

/// @brief Reads a JSON scalar as a field value.
FieldValue fieldOf(const QJsonValue& value) {
    FieldValue field;
    switch (value.type()) {
    case QJsonValue::Double:
        field.number = value.toDouble();
        break;
    case QJsonValue::Bool:
        field.flag = value.toBool();
        break;
    case QJsonValue::String:
        field.text = value.toString().toStdString();
        break;
    default:
        field.structured = true;
        break;
    }
    return field;
}

/// @brief Wraps a number as a field value.
FieldValue numberField(double number) {
    FieldValue field;
    field.number = number;
    return field;
}

/// @brief Reads a JSON array of two numbers.
std::optional<std::pair<double, double>> pairOf(const QJsonValue& value) {
    const QJsonArray array = value.toArray();
    if (!value.isArray() || array.size() != 2 || !array[0].isDouble() || !array[1].isDouble()) {
        return std::nullopt;
    }
    return std::pair{array[0].toDouble(), array[1].toDouble()};
}

/// @brief Narrows a double to a float, clamping what is too large for one.
float floatOf(double value) {
    constexpr double most = std::numeric_limits<float>::max();
    return static_cast<float>(std::clamp(value, -most, most));
}

/// @brief Reads one stroke object of a brush's `strokes`.
StrokeFields strokeFieldsOf(const QJsonValue& value) {
    StrokeFields fields;
    if (!value.isObject()) {
        fields.problem = "it is not an object";
        return fields;
    }
    const QJsonObject object = value.toObject();
    DecodedStroke decoded;
    const auto numberOf = [&](const char* key, float& into) {
        const QJsonValue entry = object.value(key);
        if (!entry.isDouble()) {
            fields.problem = std::string("'") + key +
                             (entry.isUndefined() ? "' is missing" : "' is not a number");
            return;
        }
        into = floatOf(entry.toDouble());
    };
    numberOf("radius", decoded.stroke.radius);
    numberOf("hardness", decoded.stroke.hardness);
    numberOf("flow", decoded.stroke.flow);
    if (!object.value("erase").isBool()) {
        fields.problem = "'erase' is missing or not true or false";
    }
    decoded.stroke.erase = object.value("erase").toBool();
    if (!object.value("points").isArray()) {
        fields.problem = "'points' is missing or not a list";
    } else {
        for (const QJsonValue& entry : object.value("points").toArray()) {
            const auto pair = pairOf(entry);
            if (!pair) {
                fields.problem = "a point is not a pair of numbers";
                break;
            }
            if (decoded.stroke.points.size() < maximumStrokePoints) {
                decoded.stroke.points.push_back({floatOf(pair->first), floatOf(pair->second)});
            } else {
                ++decoded.pointsDropped;
            }
        }
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString key = it.key();
        if (key != "radius" && key != "hardness" && key != "flow" && key != "erase" &&
            key != "points") {
            fields.unknownKeys.push_back(key.toStdString());
        }
    }
    if (fields.problem.empty()) {
        fields.decoded = std::move(decoded);
    }
    return fields;
}

/// @brief Reads one entry of the `masks` array as the fields it holds.
MaskFields maskFieldsOf(const QJsonValue& value) {
    MaskFields fields;
    if (!value.isObject()) {
        fields.problem = "it is not an object";
        return fields;
    }
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const std::string key = it.key().toStdString();
        if (key == "geometry") {
            if (!it.value().isObject()) {
                fields.problem = "'geometry' is not an object";
                continue;
            }
            const QJsonObject geometry = it.value().toObject();
            for (auto part = geometry.begin(); part != geometry.end(); ++part) {
                const std::string name = part.key().toStdString();
                if (name == "from" || name == "to" || name == "centre" || name == "radius") {
                    if (const auto pair = pairOf(part.value())) {
                        fields.geometry.emplace_back(name + "X", numberField(pair->first));
                        fields.geometry.emplace_back(name + "Y", numberField(pair->second));
                    } else {
                        fields.problem = "'" + name + "' is not a pair of numbers";
                    }
                } else if (name == "strokes") {
                    if (!part.value().isArray()) {
                        fields.strokesProblem = "'strokes' is not a sequence of strokes";
                        continue;
                    }
                    std::vector<StrokeFields> strokes;
                    for (const QJsonValue& stroke : part.value().toArray()) {
                        // Past the cap a stroke is only counted, so it is not decoded.
                        strokes.push_back(strokes.size() < maximumStrokesPerMask
                                              ? strokeFieldsOf(stroke)
                                              : StrokeFields{});
                    }
                    fields.strokes = std::move(strokes);
                } else {
                    fields.geometry.emplace_back(name, fieldOf(part.value()));
                }
            }
        } else if (key == "deltas") {
            if (!it.value().isObject()) {
                fields.problem = "'deltas' is not an object";
                continue;
            }
            const QJsonObject deltas = it.value().toObject();
            for (auto delta = deltas.begin(); delta != deltas.end(); ++delta) {
                fields.deltas.emplace_back(delta.key().toStdString(), fieldOf(delta.value()));
            }
        } else {
            fields.top.emplace_back(key, fieldOf(it.value()));
        }
    }
    return fields;
}

/// @brief Reports a part of the list document that cannot be read.
void reportMalformedList(std::string_view key, std::string expected, DiagnosticLog& log) {
    log.record({.notice = Notice::SettingMalformed,
                .severity = Severity::Warning,
                .values = {std::string(key), std::move(expected)}});
}

} // namespace

std::string arraw::settingsToJson(const DevelopSettings& settings) {
    return settingsDocumentHead(settings) + "\n}\n";
}

std::string arraw::stateToJson(const DevelopState& state) {
    validate(state);
    return settingsDocumentHead(state.settings) +
           ",\n  \"localAdjustments\": " + listJson(state, true, StrokeDetail::Points) + "\n}\n";
}

std::string arraw::localAdjustmentsToJson(const DevelopState& state, StrokeDetail detail) {
    validate(state);
    return listJson(state, false, detail);
}

DevelopState arraw::applyStateJson(std::string_view json, DevelopState base, DiagnosticLog& log) {
    base.settings = applySettingsJson(json, std::move(base.settings), log);
    // The settings call has checked that this is a JSON object.
    const QJsonObject root =
        QJsonDocument::fromJson(
            QByteArray::fromRawData(json.data(), static_cast<qsizetype>(json.size())))
            .object();
    if (!root.contains("localAdjustments")) {
        return base;
    }
    const QJsonValue listValue = root.value("localAdjustments");
    if (!listValue.isObject()) {
        reportMalformedList("localAdjustments", "an object", log);
        return base;
    }
    const QJsonObject list = listValue.toObject();
    if (!list.value("masks").isArray()) {
        reportMalformedList("localAdjustments.masks", "a list", log);
        return base;
    }
    if (list.contains("version")) {
        const QJsonValue version = list.value("version");
        if (!version.isDouble() || version.toDouble() < 1 ||
            version.toDouble() != std::floor(version.toDouble())) {
            reportMalformedList("localAdjustments.version", "a whole number of at least 1", log);
        } else if (version.toDouble() > localAdjustmentsVersion) {
            log.record(
                {.notice = Notice::NewerLocalAdjustmentsVersion,
                 .severity = Severity::Warning,
                 .values = {version.toDouble(), static_cast<double>(localAdjustmentsVersion)}});
        }
    }
    std::vector<MaskFields> entries;
    for (const QJsonValue& entry : list.value("masks").toArray()) {
        entries.push_back(maskFieldsOf(entry));
    }
    std::optional<double> stored;
    if (list.contains("nextId")) {
        const QJsonValue next = list.value("nextId");
        stored = next.isDouble() ? next.toDouble() : std::numeric_limits<double>::quiet_NaN();
    }
    ReadLocalAdjustments read = readLocalAdjustments(entries, stored, log, std::nullopt);
    base.localAdjustments = std::move(read.adjustments);
    base.nextLocalAdjustmentId = read.next;
    validate(base);
    return base;
}

std::string arraw::negativeToJson(const NegativeSettings& negative) {
    validate(negative);
    return "{\"demosaic\": " + jsonString(tokenOf(negative.demosaic)) + "}";
}

NegativeSettings arraw::applyNegativeJson(std::string_view json, NegativeSettings base,
                                          DiagnosticLog& log) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromRawData(json.data(), static_cast<qsizetype>(json.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::invalid_argument("negative settings are not a JSON object");
    }
    const QJsonObject root = document.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string key = it.key().toStdString();
        if (key != "demosaic") {
            log.record(
                {.notice = Notice::SettingUnknown, .severity = Severity::Warning, .values = {key}});
            continue;
        }
        const auto token = it.value().isString()
                               ? parseDemosaic(it.value().toString().toStdString())
                               : std::nullopt;
        if (token) {
            base.demosaic = *token;
        } else {
            std::string expected = "one of";
            for (const std::string_view name : demosaicTokens) {
                expected += ' ';
                expected += name;
            }
            reportMalformedList("demosaic", std::move(expected), log);
        }
    }
    return base;
}

DevelopSettings arraw::applySettingsJson(std::string_view json, DevelopSettings base,
                                         DiagnosticLog& log) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromRawData(json.data(), static_cast<qsizetype>(json.size())), &error);
    if (error.error != QJsonParseError::NoError) {
        throw std::invalid_argument("settings are not valid JSON: " +
                                    error.errorString().toStdString());
    }
    if (!document.isObject()) {
        throw std::invalid_argument("settings document is not a JSON object");
    }
    const QJsonObject root = document.object();
    const QJsonValue version = root.value("arraw");
    if (!version.isDouble() || version.toDouble() < 1 ||
        version.toDouble() != std::floor(version.toDouble())) {
        throw std::invalid_argument(
            "settings document has no integer \"arraw\" version of at least 1");
    }
    const QJsonValue settingsValue = root.value("settings");
    if (!settingsValue.isObject()) {
        throw std::invalid_argument("settings document has no \"settings\" object");
    }
    const QJsonObject entries = settingsValue.toObject();

    if (version.toDouble() > settingsJsonVersion) {
        log.record({.notice = Notice::NewerSettingsVersion,
                    .severity = Severity::Warning,
                    .values = {version.toDouble(), static_cast<double>(settingsJsonVersion)}});
    }

    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        const QString key =
            QString::fromUtf8(descriptor.key.data(), static_cast<qsizetype>(descriptor.key.size()));
        if (!entries.contains(key)) {
            continue;
        }
        if (const auto encoded = unspell(entries.value(key))) {
            decode(descriptor, *encoded, base, log);
        } else {
            reportMalformed(descriptor, log);
        }
    }
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const std::string key = it.key().toStdString();
        if (findDescriptor(key) == nullptr) {
            log.record(
                {.notice = Notice::SettingUnknown, .severity = Severity::Warning, .values = {key}});
        }
    }
    validate(base);
    return base;
}
