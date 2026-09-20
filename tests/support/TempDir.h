#pragma once

#include <QTemporaryDir>

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace arraw::test {

/// @brief Unique temporary directory, removed with its contents on destruction.
///
/// A thin adapter over QTemporaryDir that speaks `std::filesystem::path`, which
/// is what the library's own interfaces use.
class TempDir {
public:
    /// @brief Creates the directory under the system temporary location.
    /// @throws std::runtime_error if it cannot be created.
    TempDir() {
        if (!directory_.isValid()) {
            throw std::runtime_error(directory_.errorString().toStdString());
        }
    }

    /// @brief Path of the directory itself.
    [[nodiscard]] std::filesystem::path path() const {
        return std::filesystem::path(directory_.path().toStdU16String());
    }

    /// @brief Builds a path to a named entry inside the directory.
    /// @param name File name to append; the file is not created.
    /// @return The full path.
    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return path() / name;
    }

    /// @brief Keeps the directory on disk, for inspecting a failed test.
    void keep() {
        directory_.setAutoRemove(false);
    }

private:
    QTemporaryDir directory_;
};

} // namespace arraw::test
