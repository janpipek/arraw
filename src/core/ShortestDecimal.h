#pragma once

#include <charconv>
#include <string>

namespace arraw {

/// @brief Spells a double as its shortest text that reads back the same.
/// @param value Number to spell.
/// @return The text, in the form `std::to_chars` gives.
[[nodiscard]] inline std::string shortestText(double value) {
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, written.ptr);
}

/// @brief Reads a float as the double that spells it the shortest.
///
/// 0.3F becomes 0.3 rather than 0.30000001192092896.
/// @param value Number to widen.
/// @return The double with the same shortest decimal text.
[[nodiscard]] inline double shortestDouble(float value) {
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
/// @param value Number to narrow.
/// @return The float that reads back from the double's shortest text.
[[nodiscard]] inline float shortestFloat(double value) {
    const std::string text = shortestText(value);
    auto result = static_cast<float>(value);
    std::from_chars(text.data(), text.data() + text.size(), result);
    return result;
}

} // namespace arraw
