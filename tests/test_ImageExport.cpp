#include "ImageExport.h"

#include "support/TempDir.h"
#include "support/TestImages.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <vector>

using namespace arraw;

namespace {

/// @brief Reads the first bytes of a file.
/// @param path File to read.
/// @param count Number of bytes wanted.
/// @return The bytes read, which may be shorter than @p count.
std::vector<std::uint8_t> head(const std::filesystem::path& path, std::size_t count) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(count);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    return bytes;
}

/// @brief Checks that a file begins with a format's magic number.
/// @param path File to inspect.
/// @param signature Bytes the format is required to start with.
/// @return `true` if the file starts with @p signature.
bool startsWith(const std::filesystem::path& path, std::initializer_list<std::uint8_t> signature) {
    return head(path, signature.size()) == std::vector<std::uint8_t>(signature);
}

constexpr std::initializer_list<std::uint8_t> pngSignature = {0x89, 'P',  'N',  'G',
                                                              0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::initializer_list<std::uint8_t> jpegSignature = {0xFF, 0xD8, 0xFF};

} // namespace

TEST_CASE("Exporting writes the format named by the extension", "[ImageExport]") {
    const test::TempDir directory;

    SECTION("PNG") {
        const auto destination = directory.file("rainbow.png");
        exportImage(test::rainbow(), destination, {});

        REQUIRE(std::filesystem::exists(destination));
        REQUIRE(std::filesystem::file_size(destination) > 0);
        REQUIRE(startsWith(destination, pngSignature));
    }

    SECTION("JPEG") {
        const auto destination = directory.file("rainbow.jpg");
        exportImage(test::rainbow(), destination, {});

        REQUIRE(std::filesystem::file_size(destination) > 0);
        REQUIRE(startsWith(destination, jpegSignature));
    }
}

TEST_CASE("A requested format overrides the extension", "[ImageExport]") {
    const test::TempDir directory;
    const auto destination = directory.file("rainbow.dat");

    ExportOptions options;
    options.format = ImageFileFormat::Png;
    exportImage(test::rainbow(), destination, options);

    REQUIRE(startsWith(destination, pngSignature));
}

TEST_CASE("An unrecognised extension is refused", "[ImageExport]") {
    const test::TempDir directory;
    const auto image = test::rainbow();

    REQUIRE_THROWS_AS(exportImage(image, directory.file("rainbow.bmp"), {}), std::invalid_argument);
    REQUIRE_THROWS_AS(exportImage(image, directory.file("rainbow"), {}), std::invalid_argument);
}
