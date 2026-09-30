#include "SettingCodec.h"

#include <array>
#include <charconv>
#include <cmath>
#include <string_view>
#include <type_traits>

using namespace arraw;

namespace {

/// @brief Spells a double as its shortest text that reads back the same.
std::string shortestText(double value) {
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, written.ptr);
}

/// @brief Reads a float as the double that spells it the shortest.
double shortestDouble(float value) {
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
    double result = value;
    std::from_chars(buffer, written.ptr, result);
    return result;
}

/// @brief Reads a double as the float with the same shortest text, the inverse of shortestDouble.
///
/// Converting the double directly would round a second time and could land a
/// unit in the last place away from the float that was encoded.
float shortestFloat(double value) {
    const std::string text = shortestText(value);
    auto result = static_cast<float>(value);
    std::from_chars(text.data(), text.data() + text.size(), result);
    return result;
}

/// @brief Where a decode reports to, and about what.
struct Reporter {
    const FieldDescriptor& descriptor;
    DiagnosticLog& log;
    const std::optional<std::filesystem::path>& subject;

    /// @brief Reports a value the row cannot take.
    void reportMalformed() const {
        arraw::reportMalformed(descriptor, log, subject);
    }

    /// @brief Reports a number that was brought into range.
    void reportClamped(double value, double limit) const {
        log.record({.notice = Notice::SettingClamped,
                    .severity = Severity::Warning,
                    .subject = subject,
                    .values = {std::string(descriptor.key), value, limit}});
    }
};

/// @brief Reads a number when it is finite.
std::optional<double> finiteNumber(const Encoded& encoded) {
    if (const auto* number = std::get_if<double>(&encoded); number && std::isfinite(*number)) {
        return *number;
    }
    return std::nullopt;
}

/// @brief Brings a number into its row's range, reporting when that changed it.
double clampedToRange(double value, const Reporter& report) {
    if (!report.descriptor.range) {
        return value;
    }
    const auto [minimum, maximum] = *report.descriptor.range;
    if (value < minimum) {
        report.reportClamped(value, minimum);
        return minimum;
    }
    if (value > maximum) {
        report.reportClamped(value, maximum);
        return maximum;
    }
    return value;
}

/// @brief Reads a finite number and clamps it, reporting when it is not one.
std::optional<double> rangedNumber(const Encoded& encoded, const Reporter& report) {
    const std::optional<double> number = finiteNumber(encoded);
    if (!number) {
        report.reportMalformed();
        return std::nullopt;
    }
    return clampedToRange(*number, report);
}

/// @brief Reads a finite number out of a compound, or nothing when it is missing or not finite.
std::optional<double> member(const Compound& compound, std::string_view key) {
    for (const auto& [name, value] : compound) {
        if (name == key) {
            return std::isfinite(value) ? std::optional{value} : std::nullopt;
        }
    }
    return std::nullopt;
}

/// @brief Names the value of an enumeration.
template <class Enum, std::size_t N>
std::string_view nameOf(const std::array<std::pair<Enum, std::string_view>, N>& table, Enum value) {
    for (const auto& [candidate, name] : table) {
        if (candidate == value) {
            return name;
        }
    }
    return {};
}

/// @brief Finds the value of an enumeration that a name spells.
template <class Enum, std::size_t N>
std::optional<Enum> valueNamed(const std::array<std::pair<Enum, std::string_view>, N>& table,
                               const Encoded& encoded) {
    if (const auto* text = std::get_if<std::string>(&encoded)) {
        for (const auto& [value, name] : table) {
            if (name == *text) {
                return value;
            }
        }
    }
    return std::nullopt;
}

/// @brief Lists the names of an enumeration for a warning.
template <class Enum, std::size_t N>
std::string listOf(const std::array<std::pair<Enum, std::string_view>, N>& table) {
    std::string text = "one of ";
    bool first = true;
    for (const auto& [value, name] : table) {
        text += first ? "" : ", ";
        text += name;
        first = false;
    }
    return text;
}

/// @brief Reads a crop rectangle when it has exactly its four edges and the geometry plan takes it.
std::optional<UprightCropRect> cropRectangle(const Compound& compound) {
    if (compound.size() != 4) {
        return std::nullopt;
    }
    const auto left = member(compound, "left");
    const auto top = member(compound, "top");
    const auto right = member(compound, "right");
    const auto bottom = member(compound, "bottom");
    if (!left || !top || !right || !bottom) {
        return std::nullopt;
    }
    const UprightCropRect rectangle{.left = *left, .top = *top, .right = *right, .bottom = *bottom};
    return isWellFormed(rectangle) ? std::optional{rectangle} : std::nullopt;
}

/// @brief Reads a crop aspect, or nothing when it is none of the three.
std::optional<CropAspect> cropAspect(const Encoded& encoded) {
    if (const auto* keyword = std::get_if<std::string>(&encoded)) {
        if (*keyword == "free") {
            return FreeCropAspect{};
        }
        if (*keyword == "original") {
            return OriginalCropAspect{};
        }
        return std::nullopt;
    }
    if (const auto* compound = std::get_if<Compound>(&encoded); compound && compound->size() == 1) {
        if (const auto ratio = member(*compound, "ratio");
            ratio && isWellFormed(CropRatio{*ratio})) {
            return CropRatio{*ratio};
        }
    }
    return std::nullopt;
}

} // namespace

std::string arraw::expectation(const FieldDescriptor& descriptor) {
    static const DevelopSettings blank;
    return visitField(descriptor, blank, [](const auto& field) -> std::string {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
            return "a number";
        } else if constexpr (std::is_same_v<T, std::optional<float>>) {
            return "a number, or unset";
        } else if constexpr (std::is_same_v<T, bool>) {
            return "true or false";
        } else if constexpr (std::is_same_v<T, WhiteBalanceMode>) {
            return listOf(whiteBalanceModeNames);
        } else if constexpr (std::is_same_v<T, QuarterTurn>) {
            return listOf(quarterTurnNames);
        } else if constexpr (std::is_same_v<T, std::optional<UprightCropRect>>) {
            return "left, top, right and bottom numbers from 0 to 1, left below right and top "
                   "below bottom, or unset";
        } else {
            static_assert(std::is_same_v<T, CropAspect>);
            return "free, original, or a positive finite ratio";
        }
    });
}

std::vector<std::vector<std::string>> arraw::compoundShapes(const FieldDescriptor& descriptor) {
    // Asked of encode itself, with a value of each compound type, so the names
    // cannot drift from what it writes.
    DevelopSettings sample;
    std::vector<std::vector<std::string>> shapes;
    visitField(descriptor, sample, [&](auto& field) {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, std::optional<UprightCropRect>>) {
            field = UprightCropRect{};
        } else if constexpr (std::is_same_v<T, CropAspect>) {
            field = CropRatio{1.0};
        } else {
            return;
        }
        const Encoded encoded = encode(descriptor, sample);
        if (const auto* compound = std::get_if<Compound>(&encoded)) {
            std::vector<std::string> names;
            for (const auto& [name, value] : *compound) {
                names.push_back(name);
            }
            shapes.push_back(std::move(names));
        }
    });
    return shapes;
}

void arraw::reportMalformed(const FieldDescriptor& descriptor, DiagnosticLog& log,
                            const std::optional<std::filesystem::path>& subject) {
    log.record({.notice = Notice::SettingMalformed,
                .severity = Severity::Warning,
                .subject = subject,
                .values = {std::string(descriptor.key), expectation(descriptor)}});
}

Encoded arraw::encode(const FieldDescriptor& descriptor, const DevelopSettings& settings) {
    return visitField(descriptor, settings, [](const auto& field) -> Encoded {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, float>) {
            return shortestDouble(field);
        } else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, bool>) {
            return field;
        } else if constexpr (std::is_same_v<T, std::optional<float>>) {
            return field ? Encoded{shortestDouble(*field)} : Encoded{};
        } else if constexpr (std::is_same_v<T, WhiteBalanceMode>) {
            return std::string(nameOf(whiteBalanceModeNames, field));
        } else if constexpr (std::is_same_v<T, QuarterTurn>) {
            return std::string(nameOf(quarterTurnNames, field));
        } else if constexpr (std::is_same_v<T, std::optional<UprightCropRect>>) {
            if (!field) {
                return Encoded{};
            }
            return Compound{{"left", field->left},
                            {"top", field->top},
                            {"right", field->right},
                            {"bottom", field->bottom}};
        } else {
            static_assert(std::is_same_v<T, CropAspect>);
            if (std::holds_alternative<FreeCropAspect>(field)) {
                return std::string("free");
            }
            if (std::holds_alternative<OriginalCropAspect>(field)) {
                return std::string("original");
            }
            return Compound{{"ratio", std::get<CropRatio>(field).widthOverHeight}};
        }
    });
}

void arraw::decode(const FieldDescriptor& descriptor, const Encoded& encoded,
                   DevelopSettings& settings, DiagnosticLog& log,
                   const std::optional<std::filesystem::path>& subject) {
    const Reporter report{descriptor, log, subject};
    visitField(descriptor, settings, [&](auto& field) {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
            if (const auto number = rangedNumber(encoded, report)) {
                if constexpr (std::is_same_v<T, float>) {
                    field = shortestFloat(*number);
                } else {
                    field = *number;
                }
            }
        } else if constexpr (std::is_same_v<T, std::optional<float>>) {
            if (std::holds_alternative<std::monostate>(encoded)) {
                field = std::nullopt;
            } else if (const auto number = finiteNumber(encoded)) {
                field = shortestFloat(clampedToRange(*number, report));
            } else {
                report.reportMalformed();
            }
        } else if constexpr (std::is_same_v<T, bool>) {
            if (const auto* flag = std::get_if<bool>(&encoded)) {
                field = *flag;
            } else {
                report.reportMalformed();
            }
        } else if constexpr (std::is_same_v<T, WhiteBalanceMode>) {
            if (const auto mode = valueNamed(whiteBalanceModeNames, encoded)) {
                field = *mode;
            } else {
                report.reportMalformed();
            }
        } else if constexpr (std::is_same_v<T, QuarterTurn>) {
            if (const auto turn = valueNamed(quarterTurnNames, encoded)) {
                field = *turn;
            } else {
                report.reportMalformed();
            }
        } else if constexpr (std::is_same_v<T, std::optional<UprightCropRect>>) {
            if (std::holds_alternative<std::monostate>(encoded)) {
                field = std::nullopt;
                return;
            }
            const auto* compound = std::get_if<Compound>(&encoded);
            if (const auto rectangle = compound ? cropRectangle(*compound) : std::nullopt) {
                field = *rectangle;
            } else {
                report.reportMalformed();
            }
        } else {
            static_assert(std::is_same_v<T, CropAspect>);
            if (const auto aspect = cropAspect(encoded)) {
                field = *aspect;
            } else {
                report.reportMalformed();
            }
        }
    });
}
