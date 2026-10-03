#pragma once

#include <string>
#include <string_view>

namespace arraw::cli {

/// @brief Removes terminal sequences and control characters from one text field.
/// @return Printable UTF-8 text, without styling or line breaks.
[[nodiscard]] std::string terminalText(std::string_view text);

} // namespace arraw::cli
