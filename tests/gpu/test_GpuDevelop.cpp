#include "GpuDevelop.h"
#include "GpuTesting.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageImport.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

#ifndef ARRAW_TEST_DATA_DIR
#error "ARRAW_TEST_DATA_DIR must name the fixture directory; see tests/CMakeLists.txt"
#endif

using namespace arraw;
using namespace arraw::test;

namespace {

/// @brief Largest colour error of a pointwise chain followed by a resample.
///
/// The pointwise error is relative to the pixel's scale and the resample is a
/// weighted mean of pixels, so the two add at worst.
constexpr double endToEndTolerance = pointwiseRelativeTolerance + resampleTolerance;

/// @brief Largest absolute alpha difference: alpha only passes through the resample.
constexpr double endToEndAlphaTolerance = resampleTolerance;

/// @brief Largest absolute alpha difference between two RGBA float images.
double worstAlphaError(const ImageBuffer& expected, const ImageBuffer& actual) {
    const std::span<const float> want = expected.samples<float>();
    const std::span<const float> got = actual.samples<float>();
    double worst = 0.0;
    for (std::size_t index = 3; index < want.size(); index += 4) {
        worst = std::max(worst, std::abs(static_cast<double>(want[index]) - got[index]));
    }
    return worst;
}

/// @brief Requires the device's development of a source to match the CPU's.
void requireMatchesCpu(const ImageBuffer& source, const DevelopSettings& settings) {
    const ImageBuffer expected = develop(source, settings);
    const ImageBuffer actual = developOnGpu(gpuContext(), source, settings).readBack();
    REQUIRE(actual.format() == PixelFormat::RgbaF32);
    REQUIRE(actual.size() == expected.size());
    const double colour = worstColourError(expected, actual);
    const double alpha = worstAlphaError(expected, actual);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "end to end: colour %.3g, alpha %.3g\n", colour, alpha);
    }
    CAPTURE(colour, alpha, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    REQUIRE(colour <= endToEndTolerance);
    REQUIRE(alpha <= endToEndAlphaTolerance);
}

/// @brief Builds settings from a tone, a colour and a geometry, each a little of everything.
DevelopSettings combined(WhiteBalanceMode mode, GeometrySettings geometry) {
    DevelopSettings settings;
    settings.tone = {0.4F, 30.0F, 30.0F, -30.0F, 10.0F, 20.0F, 70.0F};
    settings.color = {mode, 4200.0F, 8.0F};
    settings.geometry = geometry;
    return settings;
}

/// @brief Names the geometry cases, each with more than one operation.
std::array<std::pair<const char*, GeometrySettings>, 6> geometryCases() {
    return {{
        {"straighten and auto crop", {.straighten = 7.5}},
        {"quarter-turn, flips, straighten",
         {.rotation = QuarterTurn::Clockwise90, .flipHorizontal = true, .straighten = -12.0}},
        {"half-turn, vertical flip, explicit crop",
         {.rotation = QuarterTurn::Clockwise180,
          .flipVertical = true,
          .straighten = 3.0,
          .crop = {.rectangle =
                       UprightCropRect{.left = 0.1, .top = 0.2, .right = 0.8, .bottom = 0.9}}}},
        {"three quarters and a ratio crop",
         {.rotation = QuarterTurn::Clockwise270,
          .flipHorizontal = true,
          .flipVertical = true,
          .straighten = 20.0,
          .crop = {.aspect = CropRatio{3.0 / 2.0}}}},
        {"flips only", {.flipHorizontal = true, .flipVertical = true}},
        {"nothing", {}},
    }};
}

/// @brief Loads a fixture with its own encoding and orientation.
ImageBuffer fixtureImage(const char* name) {
    return loadImage(std::filesystem::path(ARRAW_TEST_DATA_DIR) / name);
}

} // namespace

TEST_CASE("Developing on the GPU matches the CPU on RAW fixtures", "[gpu][develop]") {
    const std::array fixtures{"linear-32x24-neutral.dng", "linear-32x24-warmwb.dng",
                              "linear-32x24-nowb.dng",    "linear-32x24-rotated.dng",
                              "linear-32x24-skewed.dng",  "bayer-32x24.dng"};
    for (const char* name : fixtures) {
        const ImageBuffer source = fixtureImage(name);
        for (const auto& [label, geometry] : geometryCases()) {
            for (const WhiteBalanceMode mode :
                 {WhiteBalanceMode::AsShot, WhiteBalanceMode::Custom}) {
                DYNAMIC_SECTION(name << ", " << label << ", "
                                     << (mode == WhiteBalanceMode::Custom ? "custom" : "as shot")) {
                    requireMatchesCpu(source, combined(mode, geometry));
                }
            }
        }
    }
}

TEST_CASE("Developing on the GPU matches the CPU on PNG fixtures", "[gpu][develop]") {
    // Colour settings do not apply to a non-RAW photograph; tone and geometry do.
    for (const char* name : {"testcard-61x41-srgb8.png", "testcard-61x41-srgb16.png",
                             "testcard-61x41-alpha8.png", "testcard-61x41-adobergb8.png"}) {
        const ImageBuffer source = fixtureImage(name);
        for (const auto& [label, geometry] : geometryCases()) {
            DYNAMIC_SECTION(name << ", " << label) {
                requireMatchesCpu(source, combined(WhiteBalanceMode::AsShot, geometry));
            }
        }
    }
}

TEST_CASE("Developing on the GPU with default settings matches the CPU", "[gpu][develop]") {
    for (const char* name : {"testcard-61x41-srgb8.png", "linear-32x24-rotated.dng"}) {
        DYNAMIC_SECTION(name) {
            requireMatchesCpu(fixtureImage(name), DevelopSettings{});
        }
    }
}
