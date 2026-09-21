#pragma once

#include <QImage>

#include <cstdint>
#include <filesystem>
#include <iosfwd>

namespace arraw::test {

/// @brief One pixel's channel values, as 8-bit codes.
struct Rgba8 {
    int red = 0;
    int green = 0;
    int blue = 0;
    int alpha = 0;

    friend bool operator==(const Rgba8&, const Rgba8&) = default;
    friend std::ostream& operator<<(std::ostream& stream, const Rgba8& colour);
};

/// @brief How far apart two images are, channel by channel.
///
/// Both a worst case and a shape: a maximum bound alone cannot tell a single
/// ringing artefact from a global shift, and a mean alone cannot tell a clean
/// image with one ruined region from a slightly noisy one.
struct ImageDifference {
    int maxAbsDiff = 0;            ///< Largest absolute channel difference, in 8-bit codes.
    double meanAbsDiff = 0.0;      ///< Mean absolute channel difference over every channel.
    std::uint64_t exactPixels = 0; ///< Pixels whose four channels all match exactly.
    std::uint64_t pixelCount = 0;  ///< Pixels compared.
    int worstX = 0;                ///< Column of the pixel holding @ref maxAbsDiff.
    int worstY = 0;                ///< Row of the pixel holding @ref maxAbsDiff.
    Rgba8 worstExpected;           ///< Expected colour at that pixel.
    Rgba8 worstActual;             ///< Actual colour at that pixel.

    /// @brief Fraction of pixels that match exactly, in [0, 1].
    [[nodiscard]] double exactFraction() const;

    friend std::ostream& operator<<(std::ostream& stream, const ImageDifference& difference);
};

/// @brief Decodes an image file into 8-bit sRGB RGBA, for comparison.
///
/// Deliberately goes through QImage rather than ::arraw::loadImage: an
/// integration test needs an oracle that is not the code under test.
/// @param path Image file to decode.
/// @return The decoded image, always in `QImage::Format_RGBA8888`.
/// @throws std::runtime_error if the file cannot be decoded.
[[nodiscard]] QImage decodeAsSrgb8(const std::filesystem::path& path);

/// @brief Compares two decoded images channel by channel.
/// @param expected Reference image.
/// @param actual Image under test; must have the same dimensions.
/// @return The measured difference.
/// @throws std::invalid_argument if the images differ in size or are null.
[[nodiscard]] ImageDifference compare(const QImage& expected, const QImage& actual);

/// @brief Checks whether two colours agree on every channel to within a bound.
/// @param actual Colour under test.
/// @param expected Colour it should match.
/// @param tolerance Largest acceptable per-channel difference, in 8-bit codes.
/// @return `true` if no channel differs by more than @p tolerance.
[[nodiscard]] bool within(const Rgba8& actual, const Rgba8& expected, int tolerance);

/// @brief Reads one pixel of a decoded image.
/// @param image Image to sample; must be `QImage::Format_RGBA8888`.
/// @param x Column to read.
/// @param y Row to read.
/// @return The pixel's channel values.
/// @throws std::out_of_range if the coordinates fall outside the image.
[[nodiscard]] Rgba8 pixelAt(const QImage& image, int x, int y);

} // namespace arraw::test
