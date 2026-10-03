#include "SettingsJson.h"

#include "SettingCodec.h"
#include "ShortestDecimal.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>
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

std::string arraw::settingsToJson(const DevelopSettings& settings) {
    validate(settings);
    std::string text =
        "{\n  \"arraw\": " + std::to_string(settingsJsonVersion) + ",\n  \"settings\": {\n";
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        text += "    \"" + std::string(descriptor.key) +
                "\": " + encodedToJson(encode(descriptor, settings));
        text += &descriptor == &developSettingDescriptors.back() ? "\n" : ",\n";
    }
    return text + "  }\n}\n";
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
