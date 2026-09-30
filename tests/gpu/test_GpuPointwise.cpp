#include "GpuDevelop.h"
#include "GpuPlan.h"
#include "GpuTesting.h"
#include "ProcessingPlan.h"
#include "SampleConversion.h"

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
void requireClose(const ImageBuffer& expected, const ImageBuffer& actual) {
    const double error = worstColourError(expected, actual);
    CAPTURE(error, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    REQUIRE(error <= pointwiseRelativeTolerance);
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
    const ProcessingPlan plan = planFor(source, settings);
    const ImageBuffer floats = toRgbaF32(source);
    ImageBuffer result(source.size(), PixelFormat::RgbaF32, workingEncoding);
    const std::span<const float> in = floats.samples<float>();
    const std::span<float> out = result.samples<float>();
    for (std::size_t pixel = 0; pixel < in.size() / 4; ++pixel) {
        const Colour developed =
            developPixel(plan, {in[pixel * 4], in[pixel * 4 + 1], in[pixel * 4 + 2]});
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
        developOnGpu(gpuContext(), source, settings, Stage::Pointwise);
    REQUIRE(checkpoint.isResident());
    REQUIRE(checkpoint.size() == source.size());
    return checkpoint.readBack();
}

/// @brief Requires the device's pointwise result to match the CPU chain's.
///
/// For a Normal-oriented source with default geometry `arraw::develop` is the
/// pointwise result exactly, which also checks the reference itself.
void requireMatchesCpu(const ImageBuffer& source, const DevelopSettings& settings) {
    const ImageBuffer expected = cpuPointwise(source, settings);
    if (source.orientation() == ImageOrientation::Normal &&
        settings.geometry == GeometrySettings{}) {
        const ImageBuffer developed = develop(source, settings);
        REQUIRE(compareFloat(developed, expected).bitExact);
    }
    const ImageBuffer actual = gpuPointwise(source, settings);
    REQUIRE(actual.format() == PixelFormat::RgbaF32);
    REQUIRE(actual.size() == source.size());
    requireSameAlpha(expected, actual);
    requireClose(expected, actual);
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
            requireMatchesCpu(source, settings);
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
        requireMatchesCpu(source, withTone(neutralTone()));
    }
    SECTION("as shot, default settings") {
        requireMatchesCpu(source, DevelopSettings{});
    }
    SECTION("as shot, every tone control") {
        for (const auto& [name, settings] : toneCases()) {
            CAPTURE(name);
            requireMatchesCpu(source, settings);
        }
    }
    SECTION("custom white balance and tone") {
        DevelopSettings settings = withTone({0.3F, 20.0F, 30.0F, -20.0F, 10.0F, 10.0F, 40.0F});
        settings.color = {WhiteBalanceMode::Custom, 3200.0F, 12.0F};
        requireMatchesCpu(source, settings);
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
    SECTION("blacks up") {
        tone.blacks = strongestToneControl;
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
    requireMatchesCpu(source, withTone(tone));
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
    SECTION("no tone, no shoulder") {}
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
    requireMatchesCpu(source, withTone(tone));
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
    DevelopSettings toned = withTone({0.4F, 30.0F, 30.0F, -30.0F, 10.0F, 20.0F, 70.0F});
    toned.color = {WhiteBalanceMode::Custom, 4200.0F, 8.0F};

    // As shot with the default shoulder, then a custom balance alone, tinted,
    // and under strong tone.
    const std::array<std::pair<const char*, DevelopSettings>, 4> settingsCases{{
        {"as shot", DevelopSettings{}},
        {"custom warm", warm},
        {"custom cool with tint", coolTinted},
        {"custom with tone", toned},
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
    settings.color = {WhiteBalanceMode::Custom, 3600.0F, 6.0F};
    const ProcessingPlan plan = planFor(source, settings);
    REQUIRE(plan.shapesTone);

    const DeviceImage input = context.upload(source);
    const std::span<const float> in = source.samples<float>();

    struct Probe {
        const char* name;
        PointwiseProbe probe;
        Colour (*stage)(const ProcessingPlan&, Colour);
    };
    const std::array<Probe, 4> probes{{
        {"after the matrix", PointwiseProbe::AfterMatrix,
         [](const ProcessingPlan& p, Colour c) { return p.toWorking * c; }},
        {"after exposure", PointwiseProbe::AfterExposure,
         [](const ProcessingPlan& p, Colour c) {
             c = p.toWorking * c;
             return Colour{c[0] * p.exposureGain, c[1] * p.exposureGain, c[2] * p.exposureGain};
         }},
        {"after tone", PointwiseProbe::AfterTone,
         [](const ProcessingPlan& p, Colour c) {
             c = p.toWorking * c;
             c = {c[0] * p.exposureGain, c[1] * p.exposureGain, c[2] * p.exposureGain};
             return shapeTone(p, c);
         }},
        {"developed", PointwiseProbe::Developed,
         [](const ProcessingPlan& p, Colour c) { return developPixel(p, c); }},
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

            const GpuPointwiseBlock block = packPointwise(plan, probe.probe);
            const ImageBuffer actual = context
                                           .render(GpuPass::Pointwise, bytesOf(block), input,
                                                   source.size(), workingEncoding)
                                           .readBack();
            requireSameAlpha(expected, actual);
            requireClose(expected, actual);
        }
    }
}
