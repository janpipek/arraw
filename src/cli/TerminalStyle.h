#pragma once

#include <cstdlib>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace arraw::cli {

/// @brief Semantic terminal accents.
enum class Accent { Heading, Success, Warning, Error, Muted };

/// @brief Checks whether a standard stream supports terminal decoration.
inline bool terminalStyle(const std::ostream& stream) {
    if (std::getenv("NO_COLOR") != nullptr) {
        return false;
    }
    const char* term = std::getenv("TERM");
    if (term != nullptr && std::string_view(term) == "dumb") {
        return false;
    }
    const int descriptor = &stream == &std::cout ? 1 : (&stream == &std::cerr ? 2 : -1);
    if (descriptor < 0) {
        return false;
    }
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(descriptor == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    DWORD mode = 0;
    return _isatty(descriptor) && GetConsoleMode(handle, &mode) &&
           (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    return isatty(descriptor) != 0;
#endif
}

/// @brief Wraps text in an ANSI accent when enabled.
inline std::string accented(std::string_view text, Accent accent, bool enabled) {
    if (!enabled) {
        return std::string(text);
    }
    std::string_view code;
    switch (accent) {
    case Accent::Heading:
        code = "\033[1;36m";
        break;
    case Accent::Success:
        code = "\033[1;32m";
        break;
    case Accent::Warning:
        code = "\033[1;33m";
        break;
    case Accent::Error:
        code = "\033[1;31m";
        break;
    case Accent::Muted:
        code = "\033[2m";
        break;
    }
    return std::string(code) + std::string(text) + "\033[0m";
}

/// @brief Accents text for the destination stream.
inline std::string accented(std::ostream& stream, std::string_view text, Accent accent) {
    return accented(text, accent, terminalStyle(stream));
}

/// @brief Writes parser help with highlighted section headings and option names.
inline void writeStyledHelp(std::ostream& stream, std::string_view text) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        if (line.starts_with("Usage:") || line == "Options:" || line == "Arguments:") {
            stream << accented(stream, line, Accent::Heading);
        } else if (line.starts_with("  -")) {
            const auto split = line.find("  ", 2);
            stream << accented(stream, line.substr(0, split), Accent::Success);
            if (split != std::string_view::npos) {
                stream << line.substr(split);
            }
        } else {
            stream << line;
        }
        if (end == std::string_view::npos) {
            break;
        }
        stream << '\n';
        text.remove_prefix(end + 1);
    }
}

} // namespace arraw::cli
