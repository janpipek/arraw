#include "ColorAdjustments.h"
#include "GpuDevelop.h"
#include "GpuPlan.h"
#include "GpuTesting.h"
#include "ProcessingPlan.h"
#include "SampleConversion.h"
#include "ToneCurve.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImageOrientation.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifndef ARRAW_TEST_DATA_DIR
#error "ARRAW_TEST_DATA_DIR must name the fixture directory; see tests/CMakeLists.txt"
#endif

using namespace arraw;
using namespace arraw::test;

namespace {

/// @brief Views a uniform block as the bytes a pass takes.
template <typename Block> std::span<const std::byte> bytesOf(const Block& block) {
    return std::as_bytes(std::span(&block, 1));
}

/// @brief Compares a GPU result to its CPU reference and records the error.
/// @param tolerance Largest relative error allowed; ill-conditioned input gets a looser one.
void requireClose(const ImageBuffer& expected, const ImageBuffer& actual,
                  double tolerance = pointwiseRelativeTolerance) {
    const double error = worstColourError(expected, actual);
    CAPTURE(error, tolerance, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    REQUIRE(error <= tolerance);
}

/// @brief Requires two images' alpha channels to have the same bits.
void requireSameAlpha(const ImageBuffer& expected, const ImageBuffer& actual) {
    const std::span<const float> want = expected.samples<float>();
    const std::span<const float> got = actual.samples<float>();
    REQUIRE(want.size() == got.size());
    for (std::size_t index = 3; index < want.size(); index += 4) {
        CAPTURE(index / 4);
        REQUIRE(std::bit_cast<std::uint32_t>(want[index]) ==
                std::bit_cast<std::uint32_t>(got[index]));
    }
}

/// @brief Runs the CPU chain over a buffer's pixels, with no geometry.
///
/// What `arraw::develop` does before geometry: the source as RGBA float, each
/// colour through ::arraw::developPixel, alpha as it came.
ImageBuffer cpuPointwise(const ImageBuffer& source, const DevelopSettings& settings) {
    const ProcessingPlan plan = planFor(source, DevelopState{settings});
    const ImageBuffer floats = toRgbaF32(source);
    ImageBuffer result(source.size(), PixelFormat::RgbaF32, workingEncoding);
    const std::span<const float> in = floats.samples<float>();
    const std::span<float> out = result.samples<float>();
    for (std::size_t pixel = 0; pixel < in.size() / 4; ++pixel) {
        const Colour developed =
            developPixel(plan.pointwise, {in[pixel * 4], in[pixel * 4 + 1], in[pixel * 4 + 2]});
        out[pixel * 4] = developed[0];
        out[pixel * 4 + 1] = developed[1];
        out[pixel * 4 + 2] = developed[2];
        out[pixel * 4 + 3] = in[pixel * 4 + 3];
    }
    return result;
}

/// @brief Develops on the device up to the pointwise boundary and reads it back.
ImageBuffer gpuPointwise(const ImageBuffer& source, const DevelopSettings& settings) {
    const RenderCheckpoint checkpoint =
        developOnGpu(gpuContext(), source, DevelopState{settings}, Stage::Pointwise);
    REQUIRE(checkpoint.isResident());
    REQUIRE(checkpoint.size() == source.size());
    return checkpoint.readBack();
}

/// @brief Requires the device's pointwise result to match the CPU chain's.
///
/// For a Normal-oriented source with default geometry `arraw::develop` is the
/// pointwise result exactly, which also checks the reference itself.
void requireMatchesCpu(const ImageBuffer& source, const DevelopSettings& settings,
                       double tolerance = pointwiseRelativeTolerance) {
    const ImageBuffer expected = cpuPointwise(source, settings);
    if (source.orientation() == ImageOrientation::Normal &&
        settings.geometry == GeometrySettings{}) {
        const ImageBuffer developed = develop(source, DevelopState{settings});
        REQUIRE(compareFloat(developed, expected).bitExact);
    }
    const ImageBuffer actual = gpuPointwise(source, settings);
    REQUIRE(actual.format() == PixelFormat::RgbaF32);
    REQUIRE(actual.size() == source.size());
    requireSameAlpha(expected, actual);
    requireClose(expected, actual, tolerance);
}

/// @brief Draws a magnitude spread evenly across decades.
float logUniform(std::mt19937_64& random, double lowExponent, double highExponent) {
    std::uniform_real_distribution<double> exponent(lowExponent, highExponent);
    return static_cast<float>(std::pow(10.0, exponent(random)));
}

/// @brief Builds a float image of values across many decades, from black to far above white.
///
/// Row-major, with a run of hand-picked awkward colours first so that the
/// cases that matter are in every image whatever the seed does.
/// @param size Pixel dimensions.
/// @param withNegatives Whether random channels may be negative.
/// @param encoding Encoding to record.
/// @param seed Seed of the random values.
/// @param highExponent Base-ten exponent of the largest random magnitude.
ImageBuffer sweep(ImageSize size, bool withNegatives, ColorEncoding encoding = workingEncoding,
                  std::uint64_t seed = 7, double highExponent = 3.0) {
    ImageBuffer image(size, PixelFormat::RgbaF32, std::move(encoding));
    const std::span<float> samples = image.samples<float>();
    constexpr std::array<std::array<float, 3>, 12> awkward{{
        {0.0F, 0.0F, 0.0F},
        {1.0e-9F, 1.0e-9F, 1.0e-9F},
        {1.0e-6F, 0.0F, 0.0F},
        {0.18F, 0.18F, 0.18F},
        {1.0F, 1.0F, 1.0F},
        {1.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F},
        {0.0F, 0.0F, 1.0F},
        {2.0F, 4.0F, 8.0F},
        {100.0F, 50.0F, 25.0F},
        {0.02F, 0.01F, 0.005F},
        {0.5F, 0.25F, 0.75F},
    }};
    std::mt19937_64 random(seed);
    std::uniform_real_distribution<float> unit(0.0F, 1.0F);
    for (std::size_t pixel = 0; pixel < size.pixelCount(); ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            float value = pixel < awkward.size() ? awkward[pixel][channel]
                                                 : logUniform(random, -6.0, highExponent);
            if (pixel >= awkward.size() && withNegatives && unit(random) < 0.3F) {
                value = -value;
            }
            samples[pixel * 4 + channel] = value;
        }
        samples[pixel * 4 + 3] = unit(random);
    }
    return image;
}

/// @brief Builds an integer image of pseudo-random codes, with a black and a white pixel first.
template <typename Sample>
ImageBuffer codes(ImageSize size, PixelFormat format, std::uint64_t seed = 11) {
    ImageBuffer image(size, format, workingEncoding);
    const std::span<Sample> samples = image.samples<Sample>();
    std::mt19937_64 random(seed);
    std::uniform_int_distribution<int> code(0, std::numeric_limits<Sample>::max());
    for (Sample& sample : samples) {
        sample = static_cast<Sample>(code(random));
    }
    const std::size_t channels = channelCount(format);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        samples[channel] = 0;
        samples[channels + channel] = std::numeric_limits<Sample>::max();
    }
    return image;
}

/// @brief Builds settings that ask only for the given tone.
DevelopSettings withTone(ToneSettings tone) {
    DevelopSettings settings;
    settings.tone = tone;
    return settings;
}

/// @brief Builds tone settings with the shoulder off and everything else neutral.
ToneSettings neutralTone() {
    ToneSettings tone;
    tone.filmicHighlights = noFilmicHighlights;
    return tone;
}

/// @brief Names every tone setting combination the comparisons run over.
std::vector<std::pair<std::string, DevelopSettings>> toneCases() {
    std::vector<std::pair<std::string, DevelopSettings>> cases;
    const auto add = [&](std::string name, auto change) {
        ToneSettings tone = neutralTone();
        change(tone);
        cases.emplace_back(std::move(name), withTone(tone));
    };
    add("identity, no shoulder", [](ToneSettings&) {});
    add("default settings", [](ToneSettings& tone) { tone = ToneSettings{}; });
    add("exposure -5", [](ToneSettings& tone) { tone.exposure = darkestExposure; });
    add("exposure -1.5", [](ToneSettings& tone) { tone.exposure = -1.5F; });
    add("exposure +1.5", [](ToneSettings& tone) { tone.exposure = 1.5F; });
    add("exposure +5", [](ToneSettings& tone) { tone.exposure = brightestExposure; });
    add("contrast -100", [](ToneSettings& tone) { tone.contrast = flattestContrast; });
    add("contrast +100", [](ToneSettings& tone) { tone.contrast = steepestContrast; });
    add("contrast +30", [](ToneSettings& tone) { tone.contrast = 30.0F; });
    add("shadows -100", [](ToneSettings& tone) { tone.shadows = weakestToneControl; });
    add("shadows +100", [](ToneSettings& tone) { tone.shadows = strongestToneControl; });
    add("highlights -100", [](ToneSettings& tone) { tone.highlights = weakestToneControl; });
    add("highlights +100", [](ToneSettings& tone) { tone.highlights = strongestToneControl; });
    add("blacks -100", [](ToneSettings& tone) { tone.blacks = weakestToneControl; });
    add("blacks +100", [](ToneSettings& tone) { tone.blacks = strongestToneControl; });
    add("whites -100", [](ToneSettings& tone) { tone.whites = weakestToneControl; });
    add("whites +100", [](ToneSettings& tone) { tone.whites = strongestToneControl; });
    add("everything at its maximum", [](ToneSettings& tone) {
        tone = {brightestExposure,    steepestContrast,     strongestToneControl,
                strongestToneControl, strongestToneControl, strongestToneControl,
                fullFilmicHighlights};
    });
    add("everything at its minimum", [](ToneSettings& tone) {
        tone = {darkestExposure,    flattestContrast,   weakestToneControl, weakestToneControl,
                weakestToneControl, weakestToneControl, noFilmicHighlights};
    });
    add("mixed",
        [](ToneSettings& tone) { tone = {0.7F, -35.0F, 40.0F, -60.0F, 25.0F, -15.0F, 60.0F}; });
    add("shoulder 100", [](ToneSettings& tone) { tone.filmicHighlights = fullFilmicHighlights; });
    add("shoulder 1", [](ToneSettings& tone) { tone.filmicHighlights = 1.0F; });
    add("shoulder 100 with exposure +5", [](ToneSettings& tone) {
        tone.filmicHighlights = fullFilmicHighlights;
        tone.exposure = brightestExposure;
    });
    return cases;
}

/// @brief Names every colour-block setting combination the comparisons run over.
///
/// The shoulder stays at its default, so that the colour block is also checked
/// downstream of it, where it runs.
std::vector<std::pair<std::string, DevelopSettings>> colorCases() {
    std::vector<std::pair<std::string, DevelopSettings>> cases;
    const auto add = [&](std::string name, auto change) {
        DevelopSettings settings;
        change(settings);
        cases.emplace_back(std::move(name), settings);
    };
    add("saturation -100", [](DevelopSettings& s) { s.color.saturation = weakestSaturation; });
    add("saturation +100", [](DevelopSettings& s) { s.color.saturation = strongestSaturation; });
    add("saturation +35", [](DevelopSettings& s) { s.color.saturation = 35.0F; });
    add("vibrance -100", [](DevelopSettings& s) { s.color.vibrance = weakestSaturation; });
    add("vibrance +100", [](DevelopSettings& s) { s.color.vibrance = strongestSaturation; });
    add("vibrance -40", [](DevelopSettings& s) { s.color.vibrance = -40.0F; });
    add("saturation and vibrance", [](DevelopSettings& s) {
        s.color.saturation = -25.0F;
        s.color.vibrance = 60.0F;
    });
    add("hue red +100", [](DevelopSettings& s) { s.hsl.red.hue = strongestHslControl; });
    add("hue green -100", [](DevelopSettings& s) { s.hsl.green.hue = weakestHslControl; });
    add("saturation blue +100",
        [](DevelopSettings& s) { s.hsl.blue.saturation = strongestHslControl; });
    add("saturation orange -100",
        [](DevelopSettings& s) { s.hsl.orange.saturation = weakestHslControl; });
    add("luminance yellow +100",
        [](DevelopSettings& s) { s.hsl.yellow.luminance = strongestHslControl; });
    add("luminance magenta -100",
        [](DevelopSettings& s) { s.hsl.magenta.luminance = weakestHslControl; });
    add("every band", [](DevelopSettings& s) {
        s.hsl = {{20.0F, -30.0F, 10.0F},  {-40.0F, 25.0F, -15.0F}, {60.0F, 10.0F, 20.0F},
                 {-10.0F, -50.0F, 30.0F}, {35.0F, 45.0F, -25.0F},  {-70.0F, 15.0F, 5.0F},
                 {55.0F, -20.0F, -35.0F}, {-25.0F, 40.0F, 45.0F}};
    });
    add("hsl with saturation and vibrance", [](DevelopSettings& s) {
        s.hsl.red = {50.0F, 20.0F, -10.0F};
        s.hsl.aqua = {-50.0F, -20.0F, 10.0F};
        s.color.saturation = 20.0F;
        s.color.vibrance = -30.0F;
    });
    add("grayscale, flat mix",
        [](DevelopSettings& s) { s.blackAndWhite.convertToGrayscale = true; });
    add("grayscale, mixed", [](DevelopSettings& s) {
        s.blackAndWhite = {true, 60.0F, -40.0F, 80.0F, -20.0F, 30.0F, -90.0F, 50.0F, -10.0F};
    });
    add("grayscale, extremes", [](DevelopSettings& s) {
        s.blackAndWhite = {
            true,           darkestGrayMix,  lightestGrayMix, darkestGrayMix, lightestGrayMix,
            darkestGrayMix, lightestGrayMix, darkestGrayMix,  lightestGrayMix};
    });
    add("grayscale replaces the colour controls", [](DevelopSettings& s) {
        s.blackAndWhite = {true, 30.0F, 0.0F, 0.0F, -30.0F, 0.0F, 0.0F, 0.0F, 0.0F};
        s.color.saturation = 100.0F;
        s.hsl.red.hue = 100.0F;
    });
    add("a mix without the switch", [](DevelopSettings& s) { s.blackAndWhite.red = 80.0F; });
    add("grade shadows only", [](DevelopSettings& s) {
        s.colorGrading.shadows = {.hue = 230.0F, .saturation = strongestGrade};
    });
    add("grade every zone", [](DevelopSettings& s) {
        s.colorGrading = {.shadows = {.hue = 200.0F, .saturation = 60.0F},
                          .midtones = {.hue = 320.0F, .saturation = 25.0F},
                          .highlights = {.hue = 50.0F, .saturation = 80.0F},
                          .balance = 15.0F,
                          .blending = 35.0F};
    });
    add("grade sharp, toward the shadows", [](DevelopSettings& s) {
        s.colorGrading = {.shadows = {.hue = 0.0F, .saturation = 70.0F},
                          .highlights = {.hue = 180.0F, .saturation = 70.0F},
                          .balance = -gradeBalanceLimit,
                          .blending = sharpestGradeBlending};
    });
    add("grade soft, toward the highlights", [](DevelopSettings& s) {
        s.colorGrading = {.shadows = {.hue = maximumGradeHue, .saturation = 40.0F},
                          .midtones = {.hue = 140.0F, .saturation = 90.0F},
                          .balance = gradeBalanceLimit,
                          .blending = softestGradeBlending};
    });
    add("grade over grayscale", [](DevelopSettings& s) {
        s.blackAndWhite = {true, 40.0F, -20.0F, 10.0F, 30.0F, -30.0F, 20.0F, -10.0F, 0.0F};
        s.colorGrading = {.shadows = {.hue = 250.0F, .saturation = 50.0F},
                          .highlights = {.hue = 70.0F, .saturation = 60.0F},
                          .balance = -20.0F};
    });
    add("grade after the colour controls", [](DevelopSettings& s) {
        s.color.saturation = 30.0F;
        s.hsl.blue = {-40.0F, 20.0F, 10.0F};
        s.colorGrading.midtones = {.hue = 30.0F, .saturation = 45.0F};
    });
    add("grade highlights toward white", [](DevelopSettings& s) {
        s.colorGrading = {.midtones = {.hue = 90.0F, .saturation = 60.0F},
                          .highlights = {.hue = 260.0F, .saturation = strongestGrade}};
    });
    add("grade hues without saturation", [](DevelopSettings& s) {
        s.colorGrading = {.shadows = {.hue = 120.0F},
                          .midtones = {.hue = 240.0F},
                          .highlights = {.hue = 300.0F},
                          .balance = 60.0F,
                          .blending = 10.0F};
    });
    return cases;
}

/// @brief Builds a tone curve from control points.
ToneCurve curveOf(std::vector<CurvePoint> points) {
    return ToneCurve{std::move(points)};
}

/// @brief Names every tone curve combination the comparisons run over.
///
/// The shoulder and the rest stay at their defaults, so that the curves are
/// also checked upstream of the shoulder, where they run.
std::vector<std::pair<std::string, DevelopSettings>> curveCases() {
    const ToneCurve sCurve = curveOf({{0.0F, 0.0F}, {0.25F, 0.15F}, {0.75F, 0.85F}, {1.0F, 1.0F}});
    const ToneCurve lifted = curveOf({{0.0F, 0.2F}, {0.5F, 0.6F}, {1.0F, 1.0F}});
    const ToneCurve steepEnd = curveOf({{0.0F, 0.0F}, {0.6F, 0.3F}, {1.0F, 1.0F}});
    const ToneCurve inverting = curveOf({{0.0F, 1.0F}, {1.0F, 0.0F}});
    const ToneCurve wiggly = curveOf({{0.0F, 0.0F},
                                      {0.1F, 0.25F},
                                      {0.3F, 0.3F},
                                      {0.45F, 0.7F},
                                      {0.7F, 0.72F},
                                      {0.9F, 0.95F},
                                      {1.0F, 1.0F}});
    const ToneCurve flatEnd = curveOf({{0.0F, 0.0F}, {0.5F, 0.8F}, {1.0F, 0.8F}});
    ToneCurve sixteen;
    sixteen.points.clear();
    for (int i = 0; i < 16; ++i) {
        const float x = static_cast<float>(i) / 15.0F;
        sixteen.points.push_back({x, x + (i % 2 == 0 ? 0.0F : 0.03F) * (1.0F - x)});
    }

    std::vector<std::pair<std::string, DevelopSettings>> cases;
    const auto add = [&](std::string name, auto change) {
        DevelopSettings settings;
        change(settings.toneCurve);
        cases.emplace_back(std::move(name), settings);
    };
    add("luma s-curve", [&](ToneCurveSettings& c) { c.luma = sCurve; });
    add("luma lifted black", [&](ToneCurveSettings& c) { c.luma = lifted; });
    add("luma steep end", [&](ToneCurveSettings& c) { c.luma = steepEnd; });
    add("luma inverting", [&](ToneCurveSettings& c) { c.luma = inverting; });
    add("luma seven-point", [&](ToneCurveSettings& c) { c.luma = wiggly; });
    add("luma sixteen-point", [&](ToneCurveSettings& c) { c.luma = sixteen; });
    add("luma flat end", [&](ToneCurveSettings& c) { c.luma = flatEnd; });
    add("red only", [&](ToneCurveSettings& c) { c.red = sCurve; });
    add("green only", [&](ToneCurveSettings& c) { c.green = lifted; });
    add("blue only", [&](ToneCurveSettings& c) { c.blue = steepEnd; });
    add("rgb", [&](ToneCurveSettings& c) {
        c.red = sCurve;
        c.green = lifted;
        c.blue = steepEnd;
    });
    add("rgb lifted and flat end", [&](ToneCurveSettings& c) {
        c.red = lifted;
        c.green = flatEnd;
        c.blue = sixteen;
    });
    add("rgb inverting", [&](ToneCurveSettings& c) {
        c.red = inverting;
        c.blue = wiggly;
    });
    add("luma and rgb", [&](ToneCurveSettings& c) {
        c.luma = sCurve;
        c.red = lifted;
        c.green = wiggly;
        c.blue = steepEnd;
    });
    return cases;
}

/// @brief Reads a fixture RAW with its own encoding and orientation.
ImageBuffer fixtureImage(const char* name) {
    return loadImage(std::filesystem::path(ARRAW_TEST_DATA_DIR) / name);
}

/// @brief Replaces a fixture's camera matrix, keeping its white balance data.
ColorEncoding cameraWithMatrix(const Matrix3& toWorking) {
    CameraNative camera =
        std::get<CameraNative>(fixtureImage("linear-32x24-warmwb.dng").encoding());
    camera.toWorking = toWorking;
    return camera;
}

/// @brief Builds a one-row image from colours, with one alpha.
ImageBuffer row(std::span<const std::array<float, 3>> colours, float alpha) {
    ImageBuffer image({static_cast<std::uint32_t>(colours.size()), 1}, PixelFormat::RgbaF32,
                      workingEncoding);
    const std::span<float> samples = image.samples<float>();
    for (std::size_t pixel = 0; pixel < colours.size(); ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            samples[pixel * 4 + channel] = colours[pixel][channel];
        }
        samples[pixel * 4 + 3] = alpha;
    }
    return image;
}

} // namespace

TEST_CASE("The pointwise pass matches the CPU chain with identity settings", "[gpu][pointwise]") {
    SECTION("RGB u8") {
        requireMatchesCpu(codes<std::uint8_t>({23, 17}, PixelFormat::RgbU8), DevelopSettings{});
    }
    SECTION("RGBA u8") {
        requireMatchesCpu(codes<std::uint8_t>({23, 17}, PixelFormat::RgbaU8), DevelopSettings{});
    }
    SECTION("RGB u16") {
        requireMatchesCpu(codes<std::uint16_t>({23, 17}, PixelFormat::RgbU16), DevelopSettings{});
    }
    SECTION("RGBA u16") {
        requireMatchesCpu(codes<std::uint16_t>({23, 17}, PixelFormat::RgbaU16), DevelopSettings{});
    }
    SECTION("RGB f32") {
        ImageBuffer source({23, 17}, PixelFormat::RgbF32, workingEncoding);
        const std::span<float> samples = source.samples<float>();
        for (std::size_t index = 0; index < samples.size(); ++index) {
            samples[index] = static_cast<float>(index % 37) / 12.0F - 0.5F;
        }
        requireMatchesCpu(source, DevelopSettings{});
    }
    SECTION("RGBA f32") {
        requireMatchesCpu(sweep({23, 17}, true), DevelopSettings{});
    }
}

TEST_CASE("Identity settings without a shoulder reach the result bit for bit", "[gpu][pointwise]") {
    // Working encoding, exposure 0, no tone, no shoulder: the chain is one
    // multiply by the identity matrix and one by a gain of exactly one, which
    // are exact in float even when a GPU fuses them.
    const ImageBuffer source = sweep({40, 30}, true);
    const DevelopSettings settings = withTone(neutralTone());
    const ImageBuffer actual = gpuPointwise(source, settings);
    const FloatDifference difference = compareFloat(cpuPointwise(source, settings), actual);
    CAPTURE(difference);
    REQUIRE(difference.bitExact);
}

TEST_CASE("The pointwise pass matches the CPU chain for every tone control", "[gpu][pointwise]") {
    const ImageBuffer source = sweep({64, 48}, false);
    for (const auto& [name, settings] : toneCases()) {
        DYNAMIC_SECTION(name) {
            requireMatchesCpu(source, settings);
        }
    }
}

TEST_CASE("The pointwise pass matches the CPU chain for every colour control", "[gpu][pointwise]") {
    const ImageBuffer source = sweep({64, 48}, false, workingEncoding, 41, 2.0);
    for (const auto& [name, settings] : colorCases()) {
        DYNAMIC_SECTION(name) {
            requireMatchesCpu(source, settings);
        }
    }
}

TEST_CASE("The pointwise pass matches the CPU chain for colour controls on negative channels",
          "[gpu][pointwise]") {
    const ImageBuffer source = sweep({64, 48}, true, workingEncoding, 43, 2.0);
    for (const auto& [name, settings] : colorCases()) {
        DYNAMIC_SECTION(name) {
            requireMatchesCpu(source, settings, illConditionedRelativeTolerance);
        }
    }
}

TEST_CASE("The pointwise pass matches the CPU chain for every tone curve",
          "[gpu][pointwise][curve]") {
    // Up to a hundred times white, so that the extension above one is read.
    const ImageBuffer source = sweep({64, 48}, false, workingEncoding, 47, 2.0);
    for (const auto& [name, settings] : curveCases()) {
        DYNAMIC_SECTION(name) {
            // An inverting curve sends bright inputs to near black, where the
            // power back to linear amplifies the float rounding of the table
            // blend (a relative 1e-4 at an output of 1e-3): ill-conditioned
            // output, as a lifted black is, not a different formula.
            const bool inverts = name.find("inverting") != std::string::npos;
            requireMatchesCpu(source, settings,
                              inverts ? illConditionedRelativeTolerance
                                      : pointwiseRelativeTolerance);
        }
    }
}

TEST_CASE("The pointwise pass matches the CPU chain for tone curves on negative channels",
          "[gpu][pointwise][curve]") {
    const ImageBuffer source = sweep({64, 48}, true, workingEncoding, 53, 2.0);
    for (const auto& [name, settings] : curveCases()) {
        DYNAMIC_SECTION(name) {
            requireMatchesCpu(source, settings, illConditionedRelativeTolerance);
        }
    }
}

TEST_CASE("The pointwise pass matches the CPU chain for tone curves under the tone controls",
          "[gpu][pointwise][curve]") {
    const ImageBuffer source = sweep({64, 48}, false, workingEncoding, 59, 1.0);
    DevelopSettings settings = withTone({0.5F, 30.0F, 20.0F, -20.0F, 10.0F, -10.0F, 60.0F});
    settings.toneCurve.luma = curveOf({{0.0F, 0.0F}, {0.25F, 0.15F}, {0.75F, 0.85F}, {1.0F, 1.0F}});
    settings.toneCurve.green = curveOf({{0.0F, 0.1F}, {0.5F, 0.45F}, {1.0F, 1.0F}});
    settings.color.saturation = 20.0F;
    requireMatchesCpu(source, settings);
}

TEST_CASE("Default tone curves leave the GPU's result bit for bit as without them",
          "[gpu][pointwise][curve]") {
    // The curves bind a texture even when off; it must change nothing. The
    // chain here is the exact one of the test above, and the settings spell
    // the identity curves out rather than leaving them defaulted.
    const ImageBuffer source = sweep({40, 30}, true);
    const DevelopSettings plain = withTone(neutralTone());
    DevelopSettings spelledOut = plain;
    spelledOut.toneCurve.luma = curveOf({{0.0F, 0.0F}, {1.0F, 1.0F}});
    spelledOut.toneCurve.red = curveOf({{0.0F, 0.0F}, {1.0F, 1.0F}});
    REQUIRE(planFor(source, DevelopState{spelledOut}).pointwise.toneCurves == ToneCurvePlan{});

    const ImageBuffer without = gpuPointwise(source, plain);
    const ImageBuffer with = gpuPointwise(source, spelledOut);
    REQUIRE(compareFloat(without, with).bitExact);
    REQUIRE(compareFloat(cpuPointwise(source, plain), with).bitExact);

    // And with the default settings as a whole, against the same settings run
    // twice: no stray state in the curve texture reaches the pixels.
    const ImageBuffer first = gpuPointwise(source, DevelopSettings{});
    const ImageBuffer second = gpuPointwise(source, DevelopSettings{});
    REQUIRE(compareFloat(first, second).bitExact);
}

TEST_CASE("A tone curve resumes from a checkpoint like any other pointwise setting",
          "[gpu][pointwise][curve]") {
    const ImageBuffer source = sweep({32, 24}, false, workingEncoding, 61, 1.0);
    DevelopSettings curved;
    curved.toneCurve.luma = curveOf({{0.0F, 0.0F}, {0.25F, 0.15F}, {0.75F, 0.85F}, {1.0F, 1.0F}});

    // A checkpoint taken at the pointwise boundary resumes into geometry
    // without running the pass, so the curves are not uploaded again and the
    // pixels are those of the first development.
    const RenderCheckpoint first =
        developOnGpu(gpuContext(), source, DevelopState{curved}, Stage::Pointwise);
    const RenderCheckpoint resumed =
        developOnGpu(gpuContext(), first, source, DevelopState{curved}, Stage::Geometry);
    REQUIRE(compareFloat(first.readBack(), resumed.readBack()).bitExact);

    // Changing the curve is a change at the pointwise stage: a fresh render.
    DevelopSettings other = curved;
    other.toneCurve.red = curveOf({{0.0F, 0.1F}, {1.0F, 1.0F}});
    const RenderCheckpoint changed =
        developOnGpu(gpuContext(), source, DevelopState{other}, Stage::Pointwise);
    requireClose(cpuPointwise(source, other), changed.readBack());
}

TEST_CASE("The pointwise pass matches the CPU chain for colour controls on integer sources",
          "[gpu][pointwise]") {
    DevelopSettings settings;
    settings.color = {.saturation = 30.0F, .vibrance = 20.0F};
    settings.hsl.green = {25.0F, -30.0F, 15.0F};
    settings.hsl.red = {-35.0F, 40.0F, -10.0F};
    SECTION("RGB u8") {
        requireMatchesCpu(codes<std::uint8_t>({31, 29}, PixelFormat::RgbU8), settings);
    }
    SECTION("RGBA u16") {
        requireMatchesCpu(codes<std::uint16_t>({31, 29}, PixelFormat::RgbaU16), settings);
    }
    settings.blackAndWhite = {true, 40.0F, -20.0F, 10.0F, 30.0F, -30.0F, 20.0F, -10.0F, 0.0F};
    SECTION("grayscale, RGB u8") {
        requireMatchesCpu(codes<std::uint8_t>({31, 29}, PixelFormat::RgbU8), settings);
    }
    SECTION("grayscale, RGBA u16") {
        requireMatchesCpu(codes<std::uint16_t>({31, 29}, PixelFormat::RgbaU16), settings);
    }
}

TEST_CASE("The pointwise pass matches the CPU chain on integer sources under tone",
          "[gpu][pointwise]") {
    const DevelopSettings settings = withTone({0.5F, 20.0F, 30.0F, -30.0F, 10.0F, 10.0F, 50.0F});
    SECTION("RGB u8") {
        requireMatchesCpu(codes<std::uint8_t>({31, 29}, PixelFormat::RgbU8), settings);
    }
    SECTION("RGBA u8") {
        requireMatchesCpu(codes<std::uint8_t>({31, 29}, PixelFormat::RgbaU8), settings);
    }
    SECTION("RGB u16") {
        requireMatchesCpu(codes<std::uint16_t>({31, 29}, PixelFormat::RgbU16), settings);
    }
    SECTION("RGBA u16") {
        requireMatchesCpu(codes<std::uint16_t>({31, 29}, PixelFormat::RgbaU16), settings);
    }
}

TEST_CASE("The pointwise pass matches the CPU chain for values far above white",
          "[gpu][pointwise]") {
    const ImageBuffer bright = sweep({48, 32}, false, workingEncoding, 21, 5.0);
    for (const float shoulder : {noFilmicHighlights, 1.0F, 25.0F, fullFilmicHighlights}) {
        for (const float exposure : {0.0F, brightestExposure}) {
            for (const bool shapes : {false, true}) {
                ToneSettings tone;
                tone.filmicHighlights = shoulder;
                tone.exposure = exposure;
                tone.contrast = shapes ? 25.0F : 0.0F;
                tone.whites = shapes ? 50.0F : 0.0F;
                DYNAMIC_SECTION("shoulder " << shoulder << ", exposure " << exposure
                                            << (shapes ? ", shaped" : ", unshaped")) {
                    requireMatchesCpu(bright, withTone(tone));
                }
            }
        }
    }
}

TEST_CASE("The pointwise pass matches the CPU chain for negative channels", "[gpu][pointwise]") {
    const ImageBuffer source = sweep({64, 48}, true, workingEncoding, 33);
    for (const auto& [name, settings] : toneCases()) {
        DYNAMIC_SECTION(name) {
            requireMatchesCpu(source, settings, illConditionedRelativeTolerance);
        }
    }
}

TEST_CASE("The pointwise pass matches the CPU chain for a camera matrix that makes negatives",
          "[gpu][pointwise]") {
    // Saturated off-diagonals: a pure colour comes out with negative channels
    // in the working space, as a sensor colour outside Rec.2020 does.
    const ImageBuffer source = sweep({64, 48}, false,
                                     cameraWithMatrix(Matrix3{{1.7F, -0.5F, -0.2F, //
                                                               -0.4F, 1.6F, -0.2F, //
                                                               -0.1F, -0.6F, 1.7F}}),
                                     5, 0.5);

    SECTION("as shot, no tone") {
        requireMatchesCpu(source, withTone(neutralTone()), illConditionedRelativeTolerance);
    }
    SECTION("as shot, default settings") {
        requireMatchesCpu(source, DevelopSettings{}, illConditionedRelativeTolerance);
    }
    SECTION("as shot, every tone control") {
        for (const auto& [name, settings] : toneCases()) {
            CAPTURE(name);
            requireMatchesCpu(source, settings, illConditionedRelativeTolerance);
        }
    }
    SECTION("custom white balance and tone") {
        DevelopSettings settings = withTone({0.3F, 20.0F, 30.0F, -20.0F, 10.0F, 10.0F, 40.0F});
        settings.color = {WhiteBalanceMode::Custom, 3200.0F, 12.0F};
        requireMatchesCpu(source, settings, illConditionedRelativeTolerance);
    }
}

TEST_CASE("The pointwise pass lifts black as the CPU chain does", "[gpu][pointwise]") {
    // Zero, near-zero and non-positive luminance: the branch that takes the
    // shaped value of nothing and makes it neutral.
    constexpr std::array<std::array<float, 3>, 15> colours{{
        {0.0F, 0.0F, 0.0F},
        {-0.0F, -0.0F, -0.0F},
        // Denormal luminances, which GPUs flush and CPUs do not: both must
        // still lift, or scale by a ratio that is not a denormal quotient.
        {1.0e-39F, 0.0F, 0.0F},
        {2.0e-38F, 0.0F, 0.0F},
        {4.0e-38F, 4.0e-38F, 4.0e-38F},
        {1.0e-30F, 0.0F, 0.0F},
        {1.0e-12F, 1.0e-12F, 1.0e-12F},
        {1.0e-6F, 1.0e-6F, 1.0e-6F},
        {1.0e-4F, 0.0F, 0.0F},
        {-1.0F, 0.0F, 0.0F},
        {-0.5F, -0.5F, -0.5F},
        {1.0F, -1.0F, 0.0F},
        {0.0F, 0.3878F, -0.1F},
        {-1.0F, 0.0F, 4.4302F},
        {0.0F, 0.0F, 0.0F},
    }};
    const ImageBuffer source = row(colours, 0.25F);

    ToneSettings tone = neutralTone();
    ToneCurveSettings curves;
    SECTION("blacks up") {
        tone.blacks = strongestToneControl;
    }
    SECTION("luma curve lifting black") {
        curves.luma = curveOf({{0.0F, 0.2F}, {0.5F, 0.6F}, {1.0F, 1.0F}});
    }
    SECTION("luma curve keeping black") {
        curves.luma = curveOf({{0.0F, 0.0F}, {0.25F, 0.15F}, {0.75F, 0.85F}, {1.0F, 1.0F}});
    }
    SECTION("channel curves lifting black, on negative channels") {
        curves.red = curveOf({{0.0F, 0.2F}, {1.0F, 1.0F}});
        curves.green = curveOf({{0.0F, 0.0F}, {0.9F, 0.91F}, {1.0F, 1.0F}});
        curves.blue = curveOf({{0.0F, 0.1F}, {0.5F, 0.6F}, {1.0F, 0.9F}});
    }
    SECTION("blacks down") {
        tone.blacks = weakestToneControl;
    }
    SECTION("shadows up with a shoulder") {
        tone.shadows = strongestToneControl;
        tone.filmicHighlights = fullFilmicHighlights;
    }
    SECTION("contrast") {
        tone.contrast = steepestContrast;
    }
    SECTION("exposure and every control") {
        tone = {2.0F, 40.0F, 50.0F, 50.0F, 100.0F, 50.0F, 25.0F};
    }
    DevelopSettings settings = withTone(tone);
    settings.toneCurve = curves;
    requireMatchesCpu(source, settings, illConditionedRelativeTolerance);
}

TEST_CASE("The pointwise pass matches the CPU chain for curves near zero luminance and below zero",
          "[gpu][pointwise][curve]") {
    // Out of gamut with next to no luminance, either side of zero and of the
    // ratio floor, and negative channels through channel curves.
    constexpr std::array<std::array<float, 3>, 12> colours{{
        {1.0F, -0.38732F, 0.0F},
        {1.0F, -0.38745F, 0.0F},
        {1.0F, -0.38748F, 0.0F},
        {1.0F, -0.38760F, 0.0F},
        {6.1e-5F, 6.1e-5F, 6.1e-5F},
        {6.2e-5F, 6.2e-5F, 6.2e-5F},
        {1.0e-6F, 1.0e-6F, 1.0e-6F},
        {-0.1F, 0.5F, 0.5F},
        {-0.3F, -0.3F, -0.3F},
        {0.5F, -0.05F, 0.2F},
        {0.0F, 0.0F, 0.0F},
        {0.2F, 0.4F, 0.6F},
    }};
    const ImageBuffer source = row(colours, 0.5F);
    DevelopSettings settings;
    SECTION("luma lifting black") {
        settings.toneCurve.luma = curveOf({{0.0F, 0.2F}, {1.0F, 1.0F}});
    }
    SECTION("luma keeping black") {
        settings.toneCurve.luma = curveOf({{0.0F, 0.0F}, {0.5F, 0.5F}, {1.0F, 0.8F}});
    }
    SECTION("channel curves") {
        settings.toneCurve.red = curveOf({{0.0F, 0.0F}, {0.9F, 0.91F}, {1.0F, 1.0F}});
        settings.toneCurve.green = curveOf({{0.0F, 0.2F}, {1.0F, 1.0F}});
        settings.toneCurve.blue = curveOf({{0.0F, 0.1F}, {0.5F, 0.3F}, {1.0F, 0.9F}});
    }
    SECTION("all four") {
        settings.toneCurve.luma = curveOf({{0.0F, 0.15F}, {0.5F, 0.55F}, {1.0F, 1.0F}});
        settings.toneCurve.red = curveOf({{0.0F, 0.0F}, {0.9F, 0.91F}, {1.0F, 1.0F}});
        settings.toneCurve.green = curveOf({{0.0F, 0.2F}, {1.0F, 1.0F}});
    }
    requireMatchesCpu(source, settings, illConditionedRelativeTolerance);
}

TEST_CASE("The pointwise pass agrees with the CPU chain on NaN and infinity", "[gpu][pointwise]") {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr std::array<std::array<float, 3>, 10> colours{{
        {nan, 0.5F, 0.5F},
        {0.5F, nan, 0.5F},
        {nan, nan, nan},
        {inf, 0.0F, 0.0F},
        {inf, inf, inf},
        {inf, -inf, 0.0F},
        {-inf, -inf, -inf},
        {0.0F, inf, 0.0F},
        {0.5F, 0.5F, -inf},
        {0.2F, 0.4F, 0.6F},
    }};
    const ImageBuffer source = row(colours, 0.75F);

    ToneSettings tone = neutralTone();
    ToneCurveSettings curves;
    SECTION("no tone, no shoulder") {}
    SECTION("curves with a flat end") {
        // A flat end once made 0 * inf: infinity must stay infinity.
        curves.luma = curveOf({{0.0F, 0.0F}, {0.5F, 0.8F}, {1.0F, 0.8F}});
        curves.red = curves.luma;
        curves.blue = curveOf({{0.0F, 0.2F}, {1.0F, 1.0F}});
    }
    SECTION("channel curves alone") {
        curves.red = curveOf({{0.0F, 0.0F}, {0.5F, 0.8F}, {1.0F, 0.8F}});
        curves.green = curveOf({{0.0F, 0.2F}, {1.0F, 1.0F}});
        curves.blue = curveOf({{0.0F, 0.0F}, {0.9F, 0.5F}, {1.0F, 1.0F}});
    }
    SECTION("shoulder") {
        tone.filmicHighlights = fullFilmicHighlights;
    }
    SECTION("tone") {
        tone.contrast = 30.0F;
        tone.blacks = 50.0F;
    }
    SECTION("tone and shoulder") {
        tone = {1.0F, 30.0F, 20.0F, 20.0F, 20.0F, 20.0F, 60.0F};
    }
    DevelopSettings settings = withTone(tone);
    settings.toneCurve = curves;
    requireMatchesCpu(source, settings);
}

TEST_CASE("A highlight tint near white stays in range on the GPU as on the CPU",
          "[gpu][pointwise][grading]") {
    // Greys from below the fade to above white, a light colour and white
    // itself; the shoulder is off so that the grade sees them as they are.
    std::vector<std::array<float, 3>> colours;
    for (int step = 0; step <= 60; ++step) {
        const float lightness = 0.8F + 0.25F * static_cast<float>(step) / 60;
        const Colour g = fromOklab({lightness, 0.0F, 0.0F});
        colours.push_back({g[0], g[1], g[2]});
    }
    colours.push_back({0.9F, 0.9F, 0.9F});
    colours.push_back({1.0F, 1.0F, 1.0F});
    colours.push_back({1.0F, 0.95F, 0.9F});
    colours.push_back({4.0F, 4.0F, 4.0F});
    const ImageBuffer source = row(colours, 1.0F);

    for (const float hue : {30.0F, 90.0F, 260.0F}) {
        DYNAMIC_SECTION("hue " << hue) {
            DevelopSettings settings;
            settings.tone.filmicHighlights = noFilmicHighlights;
            settings.colorGrading.highlights = {.hue = hue, .saturation = strongestGrade};
            requireMatchesCpu(source, settings);

            // The review's cases: greys at L 0.95 and 1 and a linear grey of
            // 0.9, which the whole tint pushed to 1.2 to 1.7.
            const ImageBuffer actual = gpuPointwise(source, settings);
            const std::span<const float> out = actual.samples<float>();
            for (std::size_t pixel = 0; pixel < colours.size(); ++pixel) {
                const float lightness =
                    toOklab(Colour{colours[pixel][0], colours[pixel][1], colours[pixel][2]})
                        .lightness;
                const bool reviewed = std::abs(lightness - 0.95F) < 0.003F || lightness >= 0.999F ||
                                      colours[pixel][0] == 0.9F;
                if (!reviewed) {
                    continue;
                }
                const float ceiling =
                    std::max({1.0F, colours[pixel][0], colours[pixel][1], colours[pixel][2]});
                // Within the parity tolerance of the CPU's range.
                const float slack = ceiling * 1.0e-4F;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    CAPTURE(pixel, lightness, channel, out[pixel * 4 + channel]);
                    REQUIRE(out[pixel * 4 + channel] >= -slack);
                    REQUIRE(out[pixel * 4 + channel] <= ceiling + slack);
                }
                // Clearly above white: no tint at all, as on the CPU. A grey
                // whose L sits at 1 is left out, since the GPU's own cube root
                // may put it one ulp below the fade's end.
                if (lightness >= 1.0F + 1.0e-5F) {
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        REQUIRE(out[pixel * 4 + channel] == colours[pixel][channel]);
                    }
                }
            }
        }
    }
}

TEST_CASE("The pointwise pass matches the CPU chain on RAW fixtures", "[gpu][pointwise]") {
    const std::array fixtures{"linear-32x24-neutral.dng", "linear-32x24-warmwb.dng",
                              "linear-32x24-nowb.dng",    "linear-32x24-nowb-dark.dng",
                              "linear-32x24-highmax.dng", "linear-32x24-rotated.dng",
                              "linear-32x24-skewed.dng",  "bayer-32x24.dng"};
    DevelopSettings warm;
    warm.color = {WhiteBalanceMode::Custom, 3000.0F, 0.0F};
    DevelopSettings coolTinted;
    coolTinted.color = {WhiteBalanceMode::Custom, 8500.0F, -25.0F};
    DevelopSettings strong = withTone({0.4F, 30.0F, 30.0F, -30.0F, 10.0F, 20.0F, 70.0F});
    strong.color = {WhiteBalanceMode::Custom, 4200.0F, 8.0F};

    DevelopSettings coloured = withTone({0.2F, 10.0F, 10.0F, 0.0F, 0.0F, 0.0F, 40.0F});
    coloured.color = {WhiteBalanceMode::AsShot, std::nullopt, std::nullopt, 25.0F, 35.0F};
    coloured.hsl.orange = {30.0F, -20.0F, 10.0F};
    coloured.hsl.blue = {-45.0F, 30.0F, -10.0F};
    DevelopSettings grey = coloured;
    grey.blackAndWhite = {true, 20.0F, 10.0F, 0.0F, -10.0F, 0.0F, -30.0F, 0.0F, 0.0F};
    DevelopSettings toned = grey;
    toned.colorGrading = {.shadows = {.hue = 240.0F, .saturation = 45.0F},
                          .highlights = {.hue = 60.0F, .saturation = 55.0F},
                          .balance = -10.0F};

    // As shot with the default shoulder, then a custom balance alone, tinted,
    // under strong tone, and with the colour block.
    const std::array<std::pair<const char*, DevelopSettings>, 7> settingsCases{{
        {"as shot", DevelopSettings{}},
        {"custom warm", warm},
        {"custom cool with tint", coolTinted},
        {"custom with tone", strong},
        {"colour block", coloured},
        {"grayscale", grey},
        {"split-toned grayscale", toned},
    }};
    for (const char* name : fixtures) {
        const ImageBuffer source = fixtureImage(name);
        for (const auto& [label, settings] : settingsCases) {
            DYNAMIC_SECTION(name << ", " << label) {
                requireMatchesCpu(source, settings);
            }
        }
    }
}

TEST_CASE("The pointwise pass writes the CPU's value after each stage", "[gpu][pointwise][probe]") {
    GpuContext& context = gpuContext();
    // Negatives and values above white, so that the matrix, the gain, the
    // tone and the shoulder each have something to do.
    const ImageBuffer source = sweep({60, 40}, true,
                                     cameraWithMatrix(Matrix3{{1.6F, -0.4F, -0.2F, //
                                                               -0.3F, 1.5F, -0.2F, //
                                                               -0.1F, -0.5F, 1.6F}}),
                                     9, 1.5);

    DevelopSettings settings = withTone({1.2F, 25.0F, 30.0F, -25.0F, 20.0F, 15.0F, 80.0F});
    settings.color = {WhiteBalanceMode::Custom, 3600.0F, 6.0F, 30.0F, -20.0F};
    settings.hsl.red = {40.0F, 20.0F, -10.0F};
    settings.hsl.green = {-30.0F, -25.0F, 20.0F};
    settings.toneCurve.luma = curveOf({{0.0F, 0.0F}, {0.25F, 0.15F}, {0.75F, 0.85F}, {1.0F, 1.0F}});
    settings.toneCurve.blue = curveOf({{0.0F, 0.05F}, {0.5F, 0.45F}, {1.0F, 1.0F}});
    settings.colorGrading = {.shadows = {.hue = 210.0F, .saturation = 35.0F},
                             .highlights = {.hue = 40.0F, .saturation = 50.0F}};
    const ProcessingPlan plan = planFor(source, DevelopState{settings});
    REQUIRE(plan.pointwise.tone.shapesTone);
    REQUIRE(plan.pointwise.colorAdjustments.grading.active);
    REQUIRE(plan.pointwise.toneCurves.luma.active);
    REQUIRE(plan.pointwise.toneCurves.blue.active);
    REQUIRE(plan.pointwise.colorAdjustments.adjustsHsl);

    const DeviceImage input = context.upload(source);
    const DeviceImage curves = context.upload(packToneCurves(plan.pointwise.toneCurves));
    const std::span<const float> in = source.samples<float>();

    struct Probe {
        const char* name;
        PointwiseProbe probe;
        Colour (*stage)(const ProcessingPlan&, Colour);
    };
    const std::array<Probe, 6> probes{{
        {"after the matrix", PointwiseProbe::AfterMatrix,
         [](const ProcessingPlan& p, Colour c) { return p.pointwise.toWorking * c; }},
        {"after exposure", PointwiseProbe::AfterExposure,
         [](const ProcessingPlan& p, Colour c) {
             c = p.pointwise.toWorking * c;
             return Colour{c[0] * p.pointwise.tone.exposureGain,
                           c[1] * p.pointwise.tone.exposureGain,
                           c[2] * p.pointwise.tone.exposureGain};
         }},
        {"after tone", PointwiseProbe::AfterTone,
         [](const ProcessingPlan& p, Colour c) {
             c = p.pointwise.toWorking * c;
             c = {c[0] * p.pointwise.tone.exposureGain, c[1] * p.pointwise.tone.exposureGain,
                  c[2] * p.pointwise.tone.exposureGain};
             return shapeTone(p.pointwise.tone, c);
         }},
        {"after the curves", PointwiseProbe::AfterCurves,
         [](const ProcessingPlan& p, Colour c) {
             c = p.pointwise.toWorking * c;
             c = {c[0] * p.pointwise.tone.exposureGain, c[1] * p.pointwise.tone.exposureGain,
                  c[2] * p.pointwise.tone.exposureGain};
             return applyToneCurves(p.pointwise.toneCurves, shapeTone(p.pointwise.tone, c));
         }},
        {"after the shoulder", PointwiseProbe::AfterShoulder,
         [](const ProcessingPlan& p, Colour c) {
             c = p.pointwise.toWorking * c;
             c = {c[0] * p.pointwise.tone.exposureGain, c[1] * p.pointwise.tone.exposureGain,
                  c[2] * p.pointwise.tone.exposureGain};
             return rollHighlights(
                 p.pointwise.shoulderKnee,
                 applyToneCurves(p.pointwise.toneCurves, shapeTone(p.pointwise.tone, c)));
         }},
        {"developed", PointwiseProbe::Developed,
         [](const ProcessingPlan& p, Colour c) { return developPixel(p.pointwise, c); }},
    }};

    for (const Probe& probe : probes) {
        DYNAMIC_SECTION(probe.name) {
            ImageBuffer expected(source.size(), PixelFormat::RgbaF32, workingEncoding);
            const std::span<float> want = expected.samples<float>();
            for (std::size_t pixel = 0; pixel < in.size() / 4; ++pixel) {
                const Colour colour =
                    probe.stage(plan, {in[pixel * 4], in[pixel * 4 + 1], in[pixel * 4 + 2]});
                want[pixel * 4] = colour[0];
                want[pixel * 4 + 1] = colour[1];
                want[pixel * 4 + 2] = colour[2];
                want[pixel * 4 + 3] = in[pixel * 4 + 3];
            }

            const GpuPointwiseBlock block =
                packPointwise(plan.pointwise, source.size(), probe.probe);
            // No Presence: its four grids are not read, and the image stands in.
            const std::array inputs{input, curves, input, input, input, input};
            const ImageBuffer actual = context
                                           .render(GpuPass::Pointwise, bytesOf(block), inputs,
                                                   source.size(), workingEncoding)
                                           .readBack();
            requireSameAlpha(expected, actual);
            // The colour controls amplify rounding on the very dark pixels the
            // sweep's negative channels produce, as in the negative-channel tests.
            requireClose(expected, actual,
                         probe.probe == PointwiseProbe::Developed ? illConditionedRelativeTolerance
                                                                  : pointwiseRelativeTolerance);
        }
    }
}
