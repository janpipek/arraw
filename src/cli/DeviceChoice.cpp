#include "DeviceChoice.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <system_error>

using namespace arraw;

namespace {

/// @brief Lower-cases ASCII text.
std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

} // namespace

std::optional<cli::DeviceChoice> cli::parseDeviceChoice(std::string_view text) {
    const std::string name = lowered(text);
    if (name == "auto") {
        return DeviceChoice{DeviceKind::Auto, std::nullopt};
    }
    if (name == "cpu") {
        return DeviceChoice{DeviceKind::Cpu, std::nullopt};
    }
    if (name == "gpu") {
        return DeviceChoice{DeviceKind::Gpu, std::nullopt};
    }
    constexpr std::string_view prefix = "gpu";
    if (!name.starts_with(prefix)) {
        return std::nullopt;
    }
    // from_chars takes no sign, no space and no empty input, and the whole
    // rest must be consumed, so "gpu-1", "gpu 1", "gpu" + "x" are all refused.
    const std::string_view digits = std::string_view(name).substr(prefix.size());
    std::size_t number = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return DeviceChoice{DeviceKind::Gpu, number};
}

std::string cli::deviceChoices() {
    return "auto, cpu, gpu, or gpuN (N = 0, 1, ...)";
}
