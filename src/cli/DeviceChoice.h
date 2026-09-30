#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace arraw::cli {

/// @brief Which kind of device a `--device` value asks for.
enum class DeviceKind {
    Auto, ///< The GPU if it is usable; what each command does otherwise is its own.
    Cpu,  ///< The CPU, and no graphics stack.
    Gpu,  ///< A GPU, and a failure if it is not usable.
};

/// @brief What a `--device` value asks for.
struct DeviceChoice {
    /// @brief Kind of device asked for.
    DeviceKind kind = DeviceKind::Auto;

    /// @brief Adapter asked for by number, as `gpu2` does; empty for the
    /// backend's default device, as `gpu` does, and for every other kind.
    std::optional<std::size_t> adapter;

    /// @brief Compares two choices.
    bool operator==(const DeviceChoice&) const = default;
};

/// @brief Parses the text of a `--device` option.
///
/// `auto`, `cpu`, `gpu`, or `gpu` followed by plain decimal digits, in any
/// case. The number counts the adapters the chosen backend lists, so it means
/// something only together with a backend, and whether it exists is for the
/// machine to say, not the parser.
///
/// @param text The option's value.
/// @return The choice, or `std::nullopt` if the text is none.
[[nodiscard]] std::optional<DeviceChoice> parseDeviceChoice(std::string_view text);

/// @brief Lists the accepted values, for help and for errors.
[[nodiscard]] std::string deviceChoices();

} // namespace arraw::cli
