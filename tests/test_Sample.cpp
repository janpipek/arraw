#include "ColorSpaces.h"
#include "ProcessingPlan.h"
#include "SampleConversion.h"
#include "support/Fixtures.h"
#include "support/TestImages.h"

#include <CurveHistogram.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageImport.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <variant>

using namespace arraw;

namespace {

constexpr std::string_view neutralFixture = "linear-32x24-neutral.dng";
constexpr std::string_view skewedFixture = "linear-32x24-skewed.dng";
constexpr std::string_view testCard = "testcard-61x41-srgb8.png";

/// @brief Builds settings that move every stage before the curve input a little.
DevelopSettings toned() {
    DevelopSettings settings;
    settings.tone.exposure = 0.6F;
    settings.tone.contrast = 25.0F;
    settings.tone.shadows = 30.0F;
    settings.tone.highlights = -20.0F;
    settings.tone.blacks = -10.0F;
    settings.tone.whites = 15.0F;
    return settings;
}

/// @brief Builds settings that change only what comes after the curve input.
DevelopSettings afterTheTap(DevelopSettings settings) {
    settings.toneCurve.luma.points = {{0.0F, 0.1F}, {0.4F, 0.6F}, {1.0F, 0.9F}};
    settings.toneCurve.red.points = {{0.0F, 0.0F}, {0.5F, 0.7F}, {1.0F, 1.0F}};
    settings.toneCurve.blue.points = {{0.0F, 0.2F}, {1.0F, 0.8F}};
    settings.tone.filmicHighlights = 80.0F;
    settings.color.saturation = 40.0F;
    settings.color.vibrance = -30.0F;
    settings.colorGrading.shadows = {.hue = 200.0F, .saturation = 60.0F};
    return settings;
}

/// @brief Whether two buffers hold the same sample bits.
bool sameBits(const ImageBuffer& first, const ImageBuffer& second) {
    if (first.size() != second.size() || first.format() != second.format()) {
        return false;
    }
    const auto a = first.samples<float>();
    const auto b = second.samples<float>();
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (std::bit_cast<std::uint32_t>(a[index]) != std::bit_cast<std::uint32_t>(b[index])) {
            return false;
        }
    }
    return true;
}

/// @brief Builds a one-row perceptual buffer from RGBA values.
ImageBuffer perceptualRow(std::initializer_list<std::array<float, 4>> pixels) {
    ImageBuffer image({static_cast<std::uint32_t>(pixels.size()), 1}, PixelFormat::RgbaF32,
                      perceptualEncoding);
    const auto out = image.samples<float>();
    std::size_t index = 0;
    for (const auto& pixel : pixels) {
        for (const float value : pixel) {
            out[index++] = value;
        }
    }
    return image;
}

/// @brief Gives the bin a value falls in, computed independently of the engine.
std::size_t expectedBin(double value) {
    return static_cast<std::size_t>(
        std::clamp(std::floor(value * curveHistogramBins), 0.0, curveHistogramBins - 1.0));
}

} // namespace

TEST_CASE("A curve-input sample is the chain up to Basic Tone, perceptually encoded",
          "[develop][sample]") {
    for (const std::string_view name : {neutralFixture, skewedFixture, testCard}) {
        DYNAMIC_SECTION(name) {
            const ImageBuffer source = loadImage(test::fixture(name));
            const DevelopState state{toned()};
            const ImageBuffer tapped = sample(source, state, Tap::CurveInput);

            REQUIRE(tapped.format() == workingFormat);
            REQUIRE(std::get<NamedEncoding>(tapped.encoding()) == perceptualEncoding);
            REQUIRE(tapped.size() == source.size());

            // The stages spelled out here rather than through developToCurveInput,
            // so that a change to the tap's composition fails.
            const ProcessingPlan plan = planFor(source, state);
            const ImageBuffer floats = toRgbaF32(source);
            const auto in = floats.samples<float>();
            const auto out = tapped.samples<float>();
            for (std::size_t pixel = 0; pixel < in.size() / 4; ++pixel) {
                Colour colour{in[pixel * 4], in[pixel * 4 + 1], in[pixel * 4 + 2]};
                colour = plan.toWorking * colour;
                colour = {colour[0] * plan.exposureGain, colour[1] * plan.exposureGain,
                          colour[2] * plan.exposureGain};
                colour = shapeTone(plan, colour);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    CAPTURE(pixel, channel);
                    REQUIRE(out[pixel * 4 + channel] == toPerceptualSigned(colour[channel]));
                }
                REQUIRE(out[pixel * 4 + 3] == in[pixel * 4 + 3]);
            }
        }
    }
}

TEST_CASE("A curve-input sample covers the frame and size a render does", "[develop][sample]") {
    // With nothing after the tap that changes a colour, the developed render is
    // the tap in linear light: the same geometry and resize ran on both.
    const ImageBuffer source = loadImage(test::fixture(testCard));
    DevelopSettings settings = toned();
    settings.tone.filmicHighlights = noFilmicHighlights;
    settings.geometry = {.rotation = QuarterTurn::Clockwise90,
                         .straighten = 6.0,
                         .crop = {.rectangle = UprightCropRect{0.1, 0.05, 0.9, 0.8}}};
    const DevelopState state{settings};
    for (const auto filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        const RenderRequest request{.size = RenderRequest::FitInside{17, 17},
                                    .region = RenderRequest::Region{0.1, 0.0, 1.0, 0.9},
                                    .filter = filter};
        const ImageBuffer developed = develop(source, state, request);
        const ImageBuffer tapped = sample(source, state, Tap::CurveInput, request);
        REQUIRE(tapped.size() == developed.size());
        REQUIRE(tapped.orientation() == ImageOrientation::Normal);
        const auto want = developed.samples<float>();
        const auto got = tapped.samples<float>();
        for (std::size_t index = 0; index < want.size(); ++index) {
            CAPTURE(index);
            if (index % 4 == 3) {
                REQUIRE(got[index] == want[index]);
            } else {
                REQUIRE(got[index] == toPerceptualSigned(want[index]));
            }
        }
    }
}

TEST_CASE("The curve-input tap ignores everything after it", "[develop][sample][histogram]") {
    for (const std::string_view name : {neutralFixture, testCard}) {
        DYNAMIC_SECTION(name) {
            const ImageBuffer source = loadImage(test::fixture(name));
            const DevelopState before{toned()};
            const DevelopState after{afterTheTap(toned())};
            REQUIRE_FALSE(develop(source, before).samples<float>()[0] ==
                          develop(source, after).samples<float>()[0]);

            REQUIRE(sameBits(sample(source, before, Tap::CurveInput),
                             sample(source, after, Tap::CurveInput)));
            REQUIRE(curveHistogram(source, before) == curveHistogram(source, after));
        }
    }
}

TEST_CASE("The curve-input tap follows exposure, white balance and Basic Tone",
          "[develop][sample][histogram]") {
    const ImageBuffer source = loadImage(test::fixture(neutralFixture));
    const CurveHistogram base = curveHistogram(source, DevelopState{});

    DevelopSettings exposed;
    exposed.tone.exposure = 1.0F;
    DevelopSettings contrasty;
    contrasty.tone.contrast = 60.0F;
    DevelopSettings warm;
    warm.color.whiteBalance = WhiteBalanceMode::Custom;
    warm.color.temperature = 3000.0F;
    warm.color.tint = 20.0F;

    const CurveHistogram brighter = curveHistogram(source, DevelopState{exposed});
    REQUIRE(brighter != base);
    REQUIRE(curveHistogram(source, DevelopState{contrasty}) != base);
    const CurveHistogram balanced = curveHistogram(source, DevelopState{warm});
    REQUIRE(balanced.red != base.red);
    REQUIRE(balanced.blue != base.blue);

    // A brighter photograph moves the luma mass up the coordinate.
    const auto mean = [](const CurveHistogram::Bins& bins) {
        double sum = 0.0;
        double count = 0.0;
        for (std::size_t bin = 0; bin < bins.size(); ++bin) {
            sum += static_cast<double>(bin) * static_cast<double>(bins[bin]);
            count += static_cast<double>(bins[bin]);
        }
        return sum / count;
    };
    REQUIRE(mean(brighter.luma) > mean(base.luma));
}

TEST_CASE("A sample refuses what a render refuses, and taps that do not exist",
          "[develop][sample]") {
    const ImageBuffer card = loadImage(test::fixture(testCard));
    REQUIRE_THROWS_AS(sample(card, {}, static_cast<Tap>(7)), std::invalid_argument);
    const auto srgb = test::rainbow({2, 2}, PixelFormat::RgbaU8, NamedEncoding::Srgb);
    REQUIRE_THROWS_AS(sample(srgb, {}, Tap::CurveInput), std::invalid_argument);
    const RenderRequest badRegion{.region = RenderRequest::Region{0.5, 0.0, 0.5, 1.0}};
    REQUIRE_THROWS_AS(sample(card, {}, Tap::CurveInput, badRegion), std::invalid_argument);
}

TEST_CASE("Development refuses a perceptual sample as a source", "[develop][sample]") {
    const ImageBuffer card = loadImage(test::fixture(testCard));
    const ImageBuffer curveInput = sample(card, {}, Tap::CurveInput);
    REQUIRE(curveInput.encoding() == ColorEncoding{perceptualEncoding});
    REQUIRE_THROWS_AS(develop(curveInput, {}), std::invalid_argument);
    REQUIRE_THROWS_AS(sample(curveInput, {}, Tap::CurveInput), std::invalid_argument);
}

TEST_CASE("A curve histogram bins the perceptual coordinate", "[histogram]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const float mid = (100.0F + 0.5F) / 256.0F; // the middle of bin 100
    const ImageBuffer image = perceptualRow({
        {0.0F, 0.0F, 0.0F, 1.0F},      // black: first bin
        {mid, mid, mid, 1.0F},         // grey in bin 100, luma too
        {1.0F, 1.0F, 1.0F, 1.0F},      // white: last bin
        {1.5F, infinity, 2.0F, 1.0F},  // above white: last bin
        {-0.3F, nan, -infinity, 1.0F}, // below black and NaN: first bin
        {1.0F, 0.0F, 0.0F, 0.5F},      // pure red, half transparent: counted
        {mid, mid, mid, 0.0F},         // transparent: not counted
        {mid, mid, mid, nan},          // NaN alpha: not visible, not counted
    });
    const CurveHistogram histogram = curveHistogram(image);

    REQUIRE(histogram.pixels == 6);
    REQUIRE(histogram.red[0] == 2);
    REQUIRE(histogram.red[100] == 1);
    REQUIRE(histogram.red[255] == 3);
    REQUIRE(histogram.green[0] == 3);
    REQUIRE(histogram.green[100] == 1);
    REQUIRE(histogram.green[255] == 2);
    REQUIRE(histogram.blue[0] == 3);
    REQUIRE(histogram.blue[100] == 1);
    REQUIRE(histogram.blue[255] == 2);

    // Luma: the working luminance of the decoded colour, encoded again. Pure
    // red is a dark-ish luma, not 1/3 of white.
    const double redLuma = std::pow(static_cast<double>(colorspaces::workingLuminance[0]), 1 / 2.2);
    REQUIRE(histogram.luma[0] == 2); // black, and the NaN pixel
    REQUIRE(histogram.luma[100] == 1);
    REQUIRE(histogram.luma[255] == 2);
    REQUIRE(histogram.luma[expectedBin(redLuma)] == 1);

    for (const auto* bins : {&histogram.luma, &histogram.red, &histogram.green, &histogram.blue}) {
        std::uint64_t total = 0;
        for (const std::uint64_t count : *bins) {
            total += count;
        }
        REQUIRE(total == histogram.pixels);
    }
}

TEST_CASE("A curve histogram counts any pixel layout, and only the perceptual encoding",
          "[histogram]") {
    ImageBuffer bytes({3, 1}, PixelFormat::RgbU8, perceptualEncoding);
    const auto samples = bytes.samples<std::uint8_t>();
    const std::array<std::uint8_t, 9> values{0, 0, 0, 128, 128, 128, 255, 0, 255};
    std::copy(values.begin(), values.end(), samples.begin());
    const CurveHistogram histogram = curveHistogram(bytes);
    REQUIRE(histogram.pixels == 3);
    REQUIRE(histogram.red[0] == 1);
    REQUIRE(histogram.red[expectedBin(128.0 / 255.0)] == 1);
    REQUIRE(histogram.red[255] == 1);
    REQUIRE(histogram.green[0] == 2);

    const ImageBuffer linear({1, 1}, PixelFormat::RgbaF32, workingEncoding);
    REQUIRE_THROWS_AS(curveHistogram(linear), std::invalid_argument);
    const auto srgb = test::rainbow({2, 2}, PixelFormat::RgbaU8, NamedEncoding::Srgb);
    REQUIRE_THROWS_AS(curveHistogram(srgb), std::invalid_argument);
}

TEST_CASE("Sampling and binning in one call equals the two steps, resized bilinearly",
          "[develop][histogram]") {
    const ImageBuffer source = loadImage(test::fixture(skewedFixture));
    const DevelopState state{toned()};
    const RenderRequest lanczos{.size = RenderRequest::FitInside{16, 16},
                                .filter = ResizeFilter::Lanczos3};
    RenderRequest bilinear = lanczos;
    bilinear.filter = ResizeFilter::Bilinear;
    const CurveHistogram direct = curveHistogram(source, state, lanczos);
    REQUIRE(direct == curveHistogram(sample(source, state, Tap::CurveInput, bilinear)));
    REQUIRE(direct == curveHistogram(source, state, bilinear));
    REQUIRE(direct.pixels == resolvedSize(lanczos, source.size()).pixelCount());
}

TEST_CASE("A curve histogram's end bins hold no ringing from the resize", "[develop][histogram]") {
    // A hard vertical edge between two greys well inside the end bins: dark
    // at about bin 11, light at about bin 253. Lanczos rings past both.
    const ImageSize size{64, 16};
    ImageBuffer edge(size, PixelFormat::RgbaF32, workingEncoding);
    const auto out = edge.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const float value = x < size.width / 2 ? 0.001F : 0.98F;
            float* pixel = &out[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = pixel[1] = pixel[2] = value;
            pixel[3] = 1.0F;
        }
    }
    const RenderRequest request{.size = RenderRequest::FitInside{20, 20},
                                .filter = ResizeFilter::Lanczos3};
    const auto endBins = [](const CurveHistogram& histogram) {
        return histogram.luma.front() + histogram.luma.back() + histogram.red.front() +
               histogram.red.back();
    };

    // The test means something only if Lanczos does ring here.
    REQUIRE(endBins(curveHistogram(sample(edge, {}, Tap::CurveInput, request))) > 0);
    const CurveHistogram histogram = curveHistogram(edge, {}, request);
    REQUIRE(histogram.pixels == resolvedSize(request, size).pixelCount());
    REQUIRE(endBins(histogram) == 0);
}

TEST_CASE("A curve histogram counts a bounded render by default", "[develop][histogram]") {
    REQUIRE(curveHistogramRequest.filter == ResizeFilter::Bilinear);
    REQUIRE(curveHistogramRequest.upscale == Upscale::Never);

    const ImageSize large{1500, 300};
    // No alpha channel, so every pixel is opaque and counts.
    const ImageBuffer source(large, PixelFormat::RgbF32, workingEncoding);
    const CurveHistogram histogram = curveHistogram(source, {});
    const ImageSize bounded = resolvedSize(curveHistogramRequest, large);
    REQUIRE(bounded.width == curveHistogramLongEdge);
    REQUIRE(histogram.pixels == bounded.pixelCount());

    // A source smaller than the bound is counted at its own size.
    const ImageBuffer card = loadImage(test::fixture(testCard));
    REQUIRE(curveHistogram(card, {}).pixels == card.size().pixelCount());
}

TEST_CASE("Plans agree at the curve input exactly when its sample does", "[develop][sample]") {
    const ImageBuffer source = loadImage(test::fixture(neutralFixture));
    const RenderRequest request{.size = RenderRequest::FitInside{16, 16}};
    const ProcessingPlan base = planFor(source, DevelopState{toned()}, request);

    // What follows the tap changes the plan, but not the sample.
    const ProcessingPlan curved = planFor(source, DevelopState{afterTheTap(toned())}, request);
    REQUIRE_FALSE(curved == base);
    REQUIRE(sameAtTap(base, curved, Tap::CurveInput));

    // What precedes it, and the frame, change both.
    const auto differs = [&](const DevelopSettings& settings, const RenderRequest& other) {
        return !sameAtTap(base, planFor(source, DevelopState{settings}, other), Tap::CurveInput);
    };
    DevelopSettings settings = toned();
    settings.tone.exposure = 0.7F;
    REQUIRE(differs(settings, request));
    settings = toned();
    settings.tone.shadows = 10.0F;
    REQUIRE(differs(settings, request));
    settings = toned();
    settings.color.whiteBalance = WhiteBalanceMode::Custom;
    settings.color.temperature = 3000.0F;
    REQUIRE(differs(settings, request));
    settings = toned();
    settings.geometry.straighten = 3.0;
    REQUIRE(differs(settings, request));
    REQUIRE(differs(toned(), RenderRequest{.size = RenderRequest::FitInside{12, 12}}));

    REQUIRE_FALSE(sameAtTap(base, base, static_cast<Tap>(7)));
}
