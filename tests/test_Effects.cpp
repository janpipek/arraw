#include "Effects.h"
#include "ProcessingPlan.h"
#include "support/TestImages.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <EffectsSettings.h>
#include <ImageBuffer.h>
#include <ImagePyramid.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

using namespace arraw;

/// The Effects stage and the post-crop vignette (ADR 037).

namespace {

/// @brief Settings under which the pointwise chain leaves a working-space image as it is.
DevelopSettings plainSettings(float vignette = 0.0F, float midpoint = 50.0F,
                              float feather = 50.0F) {
    DevelopSettings settings;
    settings.tone = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    settings.effects.vignette = {vignette, midpoint, feather};
    return settings;
}

/// @brief A working-space image of one grey everywhere.
ImageBuffer greyOf(ImageSize size, float value = 0.5F) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::size_t index = 0; index < samples.size(); index += 4) {
        samples[index] = value;
        samples[index + 1] = value;
        samples[index + 2] = value;
        samples[index + 3] = 1.0F;
    }
    return image;
}

/// @brief Red channel of a pixel of a working-format image.
float redAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    return image.samples<float>()[(static_cast<std::size_t>(y) * image.size().width + x) * 4];
}

void requireIdentical(const ImageBuffer& expected, const ImageBuffer& actual) {
    REQUIRE(actual.size() == expected.size());
    REQUIRE(std::ranges::equal(actual.bytes(), expected.bytes()));
}

/// @brief Largest absolute difference between two working-format images of one size.
double worstDifference(const ImageBuffer& expected, const ImageBuffer& actual) {
    REQUIRE(actual.size() == expected.size());
    const auto want = expected.samples<float>();
    const auto got = actual.samples<float>();
    double worst = 0.0;
    for (std::size_t index = 0; index < want.size(); ++index) {
        worst = std::max(worst, std::abs(static_cast<double>(want[index]) - got[index]));
    }
    return worst;
}

VignettePlan vignetteOf(float amount, float midpoint = 50.0F, float feather = 50.0F) {
    return effectsPlanFor(EffectsSettings{{amount, midpoint, feather}}).vignette;
}

} // namespace

TEST_CASE("A vignette at zero amount is off whatever its shape, and the plan stays the default",
          "[effects][vignette]") {
    REQUIRE(effectsPlanFor({}) == EffectsPlan{});
    REQUIRE_FALSE(effectsPlanFor({}).active());
    for (const float midpoint : {0.0F, 37.0F, 100.0F}) {
        for (const float feather : {0.0F, 80.0F}) {
            REQUIRE(effectsPlanFor(EffectsSettings{{0.0F, midpoint, feather}}) == EffectsPlan{});
        }
    }
    REQUIRE(effectsPlanFor(EffectsSettings{{-1.0F, 50.0F, 50.0F}}).active());
    REQUIRE_THROWS_AS(effectsPlanFor(EffectsSettings{{std::numeric_limits<float>::quiet_NaN()}}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(
        effectsPlanFor(EffectsSettings{{10.0F, std::numeric_limits<float>::infinity(), 50.0F}}),
        std::invalid_argument);
    // Out of range clamps, as every setting the maths reads.
    REQUIRE(vignetteOf(-400.0F) == vignetteOf(-100.0F));
}

TEST_CASE("Default effects add no pass and change no pixel", "[effects][develop]") {
    const ImageBuffer source = test::rainbow({40, 30}, PixelFormat::RgbaF32, workingEncoding);
    const RenderRequest request{.size = RenderRequest::FitInside{25, 25}};
    const DevelopState plain{plainSettings()};
    // A shape with no amount is the same render.
    const DevelopState shaped{plainSettings(0.0F, 10.0F, 0.0F)};
    const ImageBuffer expected = develop(source, plain, request);
    requireIdentical(expected, develop(source, shaped, request));
    requireIdentical(expected, developUntil(source, plain, Stage::Resize, request).readBack());
    const auto effects = developUntil(source, plain, Stage::Effects, request);
    REQUIRE(effects.boundary() == Stage::Effects);
    requireIdentical(expected, effects.readBack());
    REQUIRE(planFor(source, plain, request) == planFor(source, shaped, request));
}

TEST_CASE("The falloff leaves the centre alone and is strongest in the corners",
          "[effects][vignette]") {
    const VignettePlan plan = vignetteOf(-60.0F);
    REQUIRE(vignetteWeight(plan, {0.5F, 0.5F}) == 0.0F);
    for (const FramePoint corner : {FramePoint{0.0F, 0.0F}, FramePoint{1.0F, 0.0F},
                                    FramePoint{0.0F, 1.0F}, FramePoint{1.0F, 1.0F}}) {
        REQUIRE(vignetteWeight(plan, corner) == 1.0F);
    }
    // Rising along every ray from the centre, and symmetric about both axes.
    float previous = 0.0F;
    for (int step = 0; step <= 50; ++step) {
        const float t = static_cast<float>(step) / 100.0F;
        const float weight = vignetteWeight(plan, {0.5F + t, 0.5F + t});
        REQUIRE(weight >= previous);
        // Equal up to the rounding of 0.5 +- t, which is not symmetric in float.
        REQUIRE(std::abs(weight - vignetteWeight(plan, {0.5F - t, 0.5F - t})) <= 1e-5F);
        REQUIRE(std::abs(weight - vignetteWeight(plan, {0.5F - t, 0.5F + t})) <= 1e-5F);
        previous = weight;
    }
    // Fitted to the frame: the middle of every edge is at the same radius.
    REQUIRE(vignetteWeight(plan, {0.0F, 0.5F}) == vignetteWeight(plan, {0.5F, 0.0F}));

    // Midpoint moves the start outward; Feather zero is a hard edge.
    REQUIRE(vignetteWeight(vignetteOf(-60.0F, 90.0F), {0.15F, 0.5F}) <
            vignetteWeight(vignetteOf(-60.0F, 10.0F), {0.15F, 0.5F}));
    const VignettePlan hard = vignetteOf(-60.0F, 50.0F, 0.0F);
    REQUIRE(hard.hardEdge);
    for (int step = 0; step <= 100; ++step) {
        const float weight = vignetteWeight(hard, {static_cast<float>(step) / 100.0F, 0.5F});
        REQUIRE((weight == 0.0F || weight == 1.0F));
    }

    // On a rendered frame: the centre pixels are the input, the corners darkest.
    const ImageBuffer frame = develop(greyOf({41, 31}), DevelopState{plainSettings(-60.0F)});
    REQUIRE(redAt(frame, 20, 15) == 0.5F);
    float darkest = 1.0F;
    for (std::uint32_t y = 0; y < 31; ++y) {
        for (std::uint32_t x = 0; x < 41; ++x) {
            darkest = std::min(darkest, redAt(frame, x, y));
        }
    }
    REQUIRE(redAt(frame, 0, 0) == darkest);
    REQUIRE(redAt(frame, 40, 30) == darkest);
    REQUIRE(darkest < 0.5F);
}

TEST_CASE("Darkening is exactly an exposure gain in the perceptual coordinate",
          "[effects][vignette]") {
    const VignettePlan plan = vignetteOf(-100.0F);
    REQUIRE_FALSE(plan.lightens);
    // Two stops in the corners: a quarter, exactly.
    REQUIRE(applyVignette(plan, {0.8F, 0.4F, 0.1F}, 1.0F) == Colour{0.2F, 0.1F, 0.025F});
    for (const float weight : {0.1F, 0.37F, 0.5F, 0.92F, 1.0F}) {
        const float expected = std::exp2(-plan.stops * weight / 2.2F);
        for (const float value : {0.001F, 0.18F, 0.5F, 1.0F, 3.0F}) {
            const Colour out = applyVignette(plan, {value, value, value}, weight);
            const float gain = toPerceptual(out[0]) / toPerceptual(value);
            REQUIRE(std::abs(gain - expected) <= 2e-6F * expected);
        }
    }
    // Hue-preserving: the channels keep their ratios, negatives included.
    const Colour out = applyVignette(plan, {-0.1F, 0.4F, 0.2F}, 0.6F);
    REQUIRE(std::abs(out[0] / out[1] - (-0.25F)) < 1e-6F);
    REQUIRE(std::abs(out[2] / out[1] - 0.5F) < 1e-6F);
    // No falloff, no change at all.
    REQUIRE(applyVignette(plan, {0.3F, -0.2F, 7.0F}, 0.0F) == Colour{0.3F, -0.2F, 7.0F});
}

TEST_CASE("Lightening never passes white and is monotone", "[effects][vignette]") {
    for (const float amount : {5.0F, 50.0F, 100.0F}) {
        const VignettePlan plan = vignetteOf(amount);
        REQUIRE(plan.lightens);
        for (const float weight : {0.05F, 0.5F, 1.0F}) {
            float previous = -1.0F;
            for (int step = 0; step <= 1000; ++step) {
                const float value = static_cast<float>(step) / 1000.0F;
                const float out = applyVignette(plan, {value, value, value}, weight)[0];
                REQUIRE(out <= 1.0F);
                REQUIRE(out >= value * (1.0F - 1e-6F));
                REQUIRE(out >= previous);
                previous = out;
            }
        }
        // Stronger falloff lifts further.
        REQUIRE(applyVignette(plan, {0.2F, 0.2F, 0.2F}, 0.8F)[0] >
                applyVignette(plan, {0.2F, 0.2F, 0.2F}, 0.4F)[0]);
    }
    // Monotone in the amount, and continuous into darkening at zero.
    REQUIRE(applyVignette(vignetteOf(80.0F), {0.3F, 0.3F, 0.3F}, 1.0F)[0] >
            applyVignette(vignetteOf(40.0F), {0.3F, 0.3F, 0.3F}, 1.0F)[0]);
    const float up = applyVignette(vignetteOf(0.01F), {0.3F, 0.3F, 0.3F}, 1.0F)[0];
    const float down = applyVignette(vignetteOf(-0.01F), {0.3F, 0.3F, 0.3F}, 1.0F)[0];
    REQUIRE(std::abs(up - 0.3F) < 1e-4F);
    REQUIRE(std::abs(down - 0.3F) < 1e-4F);
    // White stays white, and the result of a full lift is the screen's.
    REQUIRE(std::abs(applyVignette(vignetteOf(100.0F), {1.0F, 1.0F, 1.0F}, 1.0F)[0] - 1.0F) <
            1e-6F);
}

TEST_CASE("The crop frame position follows the region and the size", "[effects][frame]") {
    const ImageBuffer source = greyOf({40, 30});
    const DevelopState state{plainSettings(-50.0F)};
    const auto mappingOf = [&](const RenderRequest& request) {
        return frameMappingOf(planFor(source, state, request));
    };
    const FrameMapping whole = mappingOf({});
    REQUIRE(whole.origin == std::array{0.0, 0.0});
    REQUIRE(whole.step == std::array{1.0 / 40.0, 1.0 / 30.0});
    REQUIRE(whole.aspect == 40.0 / 30.0);

    const FrameMapping region = mappingOf({.size = RenderRequest::Scale{2.0},
                                           .region = RenderRequest::Region{0.25, 0.5, 0.75, 1.0},
                                           .upscale = Upscale::Allowed});
    REQUIRE(region.origin == std::array{0.25, 0.5});
    REQUIRE(region.step == std::array{1.0 / 80.0, 1.0 / 60.0});
    const FramePoint first = framePointOf(region, 0, 0);
    REQUIRE(first.x == static_cast<float>(0.25 + 0.5 / 80.0));
    REQUIRE(first.y == static_cast<float>(0.5 + 0.5 / 60.0));
}

TEST_CASE("A point of the frame gets the same falloff whatever region or zoom renders it",
          "[effects][frame][region]") {
    const ImageBuffer source = greyOf({48, 32});
    const DevelopState state{plainSettings(-80.0F, 20.0F, 70.0F)};

    // At one to one, a region is exactly the pixels of the whole frame it covers.
    const ImageBuffer whole = develop(source, state);
    const ImageBuffer region =
        develop(source, state, {.region = RenderRequest::Region{0.25, 0.5, 0.75, 1.0}});
    REQUIRE(region.size() == ImageSize{24, 16});
    double worst = 0.0;
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 24; ++x) {
            worst = std::max(worst, static_cast<double>(std::abs(redAt(region, x, y) -
                                                                 redAt(whole, 12 + x, 16 + y))));
        }
    }
    REQUIRE(worst <= 1e-6);

    // Zoomed in by two, the same: the grey is uniform, so each output pixel
    // is the falloff at its own place in the frame.
    const RenderRequest zoom{.size = RenderRequest::Scale{2.0}, .upscale = Upscale::Allowed};
    RenderRequest zoomedRegion = zoom;
    zoomedRegion.region = RenderRequest::Region{0.5, 0.25, 1.0, 0.75};
    const ImageBuffer big = develop(source, state, zoom);
    const ImageBuffer part = develop(source, state, zoomedRegion);
    REQUIRE(part.size() == ImageSize{48, 32});
    worst = 0.0;
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 48; ++x) {
            worst = std::max(worst, static_cast<double>(
                                        std::abs(redAt(part, x, y) - redAt(big, 48 + x, 16 + y))));
        }
    }
    REQUIRE(worst <= 1e-6);
}

TEST_CASE("A preview level shows the same vignette as the full frame", "[effects][preview]") {
    const DevelopState state{plainSettings(-70.0F, 30.0F, 60.0F)};
    const RenderRequest request{.size = RenderRequest::FitInside{64, 64}};
    const RenderRequest oddRequest{.size = RenderRequest::FitInside{60, 60}};

    // Sizes the pyramid divides exactly: the positions are the same, so only
    // float rounding separates the two.
    const ImageBuffer source = greyOf({256, 192});
    const ImageBuffer level = halved(halved(source));
    const ImageBuffer full = develop(source, state, request);
    const ImageBuffer reduced = develop(level, state, request);
    REQUIRE(worstDifference(full, reduced) <= 1e-5);

    // Odd sizes: the reduced frame is off by up to a pixel, which moves the
    // falloff by less than its slope over a pixel.
    const ImageBuffer odd = greyOf({250, 187});
    const ImageBuffer oddLevel = halved(halved(odd));
    const ImageBuffer oddFull = develop(odd, state, oddRequest);
    const ImageBuffer oddReduced = develop(oddLevel, state, oddRequest);
    REQUIRE(oddFull.size() == oddReduced.size());
    REQUIRE(worstDifference(oddFull, oddReduced) <= 1e-2);
}

TEST_CASE("A vignette edit resumes from the resize, and a crop edit cannot",
          "[effects][checkpoint]") {
    const ImageBuffer source = test::rainbow({40, 30}, PixelFormat::RgbaF32, workingEncoding);
    const RenderRequest request{.size = RenderRequest::FitInside{24, 24}};
    const DevelopState before{plainSettings(-30.0F)};
    const DevelopState after{plainSettings(45.0F, 70.0F, 20.0F)};

    const auto resized = developUntil(source, before, Stage::Resize, request);
    const auto resumed = resumeFrom(resized, source, after, Stage::Effects, request);
    REQUIRE(resumed.boundary() == Stage::Effects);
    requireIdentical(develop(source, after, request), resumed.readBack());

    // The effects checkpoint is for its own vignette only.
    const auto effects = developUntil(source, before, Stage::Effects, request);
    REQUIRE_NOTHROW(resumeFrom(effects, source, before, Stage::Effects, request));
    REQUIRE_THROWS_AS(resumeFrom(effects, source, after, Stage::Effects, request),
                      std::invalid_argument);
    // And a different region moves every pixel's place in the frame.
    RenderRequest panned = request;
    panned.region = RenderRequest::Region{0.0, 0.0, 0.5, 0.5};
    REQUIRE_THROWS_AS(resumeFrom(effects, source, before, Stage::Effects, panned),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(resumeFrom(effects, source, before, Stage::Resize, request),
                      std::invalid_argument);
}

TEST_CASE("Effects keep alpha and act after the region and resize", "[effects][develop]") {
    ImageBuffer source = greyOf({20, 16}, 0.4F);
    const auto samples = source.samples<float>();
    for (std::size_t index = 3; index < samples.size(); index += 8) {
        samples[index] = 0.5F;
    }
    const DevelopState state{plainSettings(60.0F)};
    const ImageBuffer plain = develop(source, DevelopState{plainSettings()});
    const ImageBuffer vignetted = develop(source, state);
    const auto want = plain.samples<float>();
    const auto got = vignetted.samples<float>();
    for (std::size_t index = 3; index < want.size(); index += 4) {
        REQUIRE(got[index] == want[index]);
    }
    // Lightened corners, untouched centre.
    REQUIRE(redAt(vignetted, 0, 0) > redAt(plain, 0, 0));
    REQUIRE(redAt(vignetted, 0, 0) <= 1.0F);
}
