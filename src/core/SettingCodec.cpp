#include "SettingCodec.h"

#include "ShortestDecimal.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

using namespace arraw;

namespace {

/// @brief Reads a whole text as a number, ignoring blanks around it and one leading `+`.
std::optional<double> wholeNumber(std::string_view text) {
    constexpr std::string_view blanks = " \t\r\n";
    const std::size_t first = text.find_first_not_of(blanks);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    text = text.substr(first, text.find_last_not_of(blanks) - first + 1);
    if (text.starts_with('+')) {
        text.remove_prefix(1);
    }
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
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

/// @brief Reads a curve from its points, or nothing if malformed.
///
/// The points may come in any order and are sorted, and an end x within
/// rounding of 0 or 1 is snapped onto it (::arraw::curveFromPoints); an x
/// further out is malformed rather than clamped, since moving a point along x
/// changes which curve it is. A y outside 0 to 1 is clamped, as every number
/// is, and the warnings about it are reported only for a curve that is taken.
std::optional<ToneCurve> curveFrom(const Encoded& encoded, const Reporter& report) {
    const auto* list = std::get_if<PointList>(&encoded);
    if (list == nullptr || list->size() < minimumCurvePoints || list->size() > maximumCurvePoints) {
        return std::nullopt;
    }
    std::vector<double> clamps;
    std::vector<CurvePoint> points;
    for (const auto& [x, y] : *list) {
        // An x this far out is malformed anyway, and is refused before it
        // could overflow a float.
        if (!std::isfinite(x) || !std::isfinite(y) || x < -1.0 || x > 2.0) {
            return std::nullopt;
        }
        if (y < 0.0 || y > 1.0) {
            clamps.push_back(y);
        }
        points.push_back({shortestFloat(x), shortestFloat(std::clamp(y, 0.0, 1.0))});
    }
    std::optional<ToneCurve> curve = curveFromPoints(std::move(points));
    if (!curve) {
        return std::nullopt;
    }
    for (const double value : clamps) {
        report.reportClamped(value, value < 0.0 ? 0.0 : 1.0);
    }
    return curve;
}

} // namespace

std::optional<PointList> arraw::parsePointList(std::string_view text) {
    PointList points;
    while (true) {
        const std::size_t end = text.find(';');
        const std::string_view pair = text.substr(0, end);
        const std::size_t comma = pair.find(',');
        if (comma == std::string_view::npos) {
            return std::nullopt;
        }
        const std::optional<double> x = wholeNumber(pair.substr(0, comma));
        const std::optional<double> y = wholeNumber(pair.substr(comma + 1));
        if (!x || !y) {
            return std::nullopt;
        }
        points.emplace_back(*x, *y);
        if (end == std::string_view::npos) {
            return points;
        }
        text.remove_prefix(end + 1);
    }
}

bool arraw::takesPoints(const FieldDescriptor& descriptor) {
    static const DevelopSettings blank;
    return visitField(descriptor, blank, [](const auto& field) {
        return std::is_same_v<std::remove_cvref_t<decltype(field)>, ToneCurve>;
    });
}

std::string arraw::expectation(const FieldDescriptor& descriptor) {
    static const DevelopSettings blank;
    return visitField(descriptor, blank, [&descriptor](const auto& field) -> std::string {
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
        } else if constexpr (std::is_same_v<T, ToneCurve>) {
            return toneCurveRequirements;
        } else if constexpr (std::is_same_v<T, GrainModel>) {
            return listOf(grainModelNames);
        } else if constexpr (std::is_same_v<T, LuminanceNoiseFilter>) {
            return listOf(luminanceNoiseFilterNames);
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
            const double most = descriptor.range ? descriptor.range->maximum : maximumGrainSeed;
            return "a whole number from 0 to " + std::to_string(static_cast<std::uint64_t>(most));
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
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
            return static_cast<double>(field);
        } else if constexpr (std::is_same_v<T, GrainModel>) {
            return std::string(nameOf(grainModelNames, field));
        } else if constexpr (std::is_same_v<T, LuminanceNoiseFilter>) {
            return std::string(nameOf(luminanceNoiseFilterNames, field));
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
        } else if constexpr (std::is_same_v<T, ToneCurve>) {
            PointList list;
            for (const CurvePoint& point : field.points) {
                list.emplace_back(shortestDouble(point.x), shortestDouble(point.y));
            }
            return list;
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
        } else if constexpr (std::is_same_v<T, GrainModel>) {
            if (const auto model = valueNamed(grainModelNames, encoded)) {
                field = *model;
            } else {
                report.reportMalformed();
            }
        } else if constexpr (std::is_same_v<T, LuminanceNoiseFilter>) {
            if (const auto filter = valueNamed(luminanceNoiseFilterNames, encoded)) {
                field = *filter;
            } else {
                report.reportMalformed();
            }
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
            // An identity such as a seed, not a quantity: brought into range or
            // rounded it would name something else, so only an exact one is taken.
            const std::optional<double> number = finiteNumber(encoded);
            const double most = descriptor.range ? descriptor.range->maximum : maximumGrainSeed;
            if (number && *number >= 0.0 && *number <= most && std::floor(*number) == *number) {
                field = static_cast<std::uint32_t>(*number);
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
        } else if constexpr (std::is_same_v<T, ToneCurve>) {
            if (auto curve = curveFrom(encoded, report)) {
                field = std::move(*curve);
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

DevelopSettings arraw::withoutUnusedSettings(DevelopSettings settings, bool raw) {
    if (!raw || settings.color.whiteBalance != WhiteBalanceMode::Custom) {
        settings.color.temperature.reset();
        settings.color.tint.reset();
    }
    return settings;
}
