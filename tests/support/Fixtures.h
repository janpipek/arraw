#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef ARRAW_TEST_DATA_DIR
#error "ARRAW_TEST_DATA_DIR must name the fixture directory; see tests/CMakeLists.txt"
#endif

namespace arraw::test {

/// @brief Locates a committed fixture file.
///
/// The directory is baked in at build time rather than resolved relative to
/// the working directory, so a test runs the same from `ctest`, from an IDE,
/// and from a shell anywhere in the tree.
///
/// @param name File name inside `tests/fixtures`.
/// @return The full path to the fixture.
/// @throws std::runtime_error if the fixture is absent, naming the command
/// that regenerates it.
[[nodiscard]] inline std::filesystem::path fixture(std::string_view name) {
    auto path = std::filesystem::path(ARRAW_TEST_DATA_DIR) / name;
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("missing fixture " + path.string() +
                                 "; regenerate the fixtures with `just fixtures`");
    }
    return path;
}

} // namespace arraw::test
