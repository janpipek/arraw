#include "CheckpointState.h"
#include "ColorSpaces.h"
#include "Denoise.h"
#include "ProcessingPlan.h"
#include "RowBands.h"
#include "support/Fixtures.h"
#include "support/TestImages.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImagePyramid.h>
#include <NoiseReductionSettings.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

using namespace arraw;

/// The Denoise stage: luminance and colour noise reduction on the source (ADR 039).

namespace {

constexpr std::string_view skewedFixture = "linear-32x24-skewed.dng";

/// @brief A portable noise value in [-1, 1) for a pixel and channel.
float noiseAt(std::uint32_t x, std::uint32_t y, std::uint32_t channel) {
    std::uint32_t h = x * 0x9E3779B1U ^ (y + 0x7F4A7C15U) * 0x85EBCA77U ^ channel * 0xC2B2AE3DU;
    h ^= h >> 16;
    h *= 0x7FEB352DU;
    h ^= h >> 15;
    h *= 0x846CA68BU;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / 8388608.0F - 1.0F;
}

/// @brief A working-space image whose colour each pixel gets from a function.
ImageBuffer imageOf(ImageSize size,
                    const std::function<Colour(std::uint32_t, std::uint32_t)>& colourAt) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const Colour colour = colourAt(x, y);
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = colour[0];
            pixel[1] = colour[1];
            pixel[2] = colour[2];
            pixel[3] = 1.0F;
        }
    }
    return image;
}

/// @brief A step from dark to light grey halfway across, with luminance noise.
ImageBuffer noisyStep(ImageSize size, float amplitude) {
    return imageOf(size, [&](std::uint32_t x, std::uint32_t y) {
        const float base = x < size.width / 2 ? 0.1F : 0.6F;
        const float value = base * (1.0F + amplitude * noiseAt(x, y, 0));
        return Colour{value, value, value};
    });
}

/// @brief A flat colour with independent noise in each channel: chroma noise.
ImageBuffer chromaNoise(ImageSize size, float amplitude) {
    return imageOf(size, [&](std::uint32_t x, std::uint32_t y) {
        return Colour{0.3F * (1.0F + amplitude * noiseAt(x, y, 0)),
                      0.25F * (1.0F + amplitude * noiseAt(x, y, 1)),
                      0.2F * (1.0F + amplitude * noiseAt(x, y, 2))};
    });
}

/// @brief Colour noise whose working luminance is the same everywhere: chroma noise alone.
ImageBuffer flatLumaChromaNoise(ImageSize size, float amplitude) {
    const Colour& row = colorspaces::workingLuminance;
    const float rowSum = row[0] + row[1] + row[2];
    return imageOf(size, [&](std::uint32_t x, std::uint32_t y) {
        const Colour noise{0.3F * amplitude * noiseAt(x, y, 0),
                           0.25F * amplitude * noiseAt(x, y, 1),
                           0.2F * amplitude * noiseAt(x, y, 2)};
        // Takes the noise's own luminance back out along the neutral.
        const float luma = (row[0] * noise[0] + row[1] * noise[1] + row[2] * noise[2]) / rowSum;
        return Colour{0.3F + noise[0] - luma, 0.25F + noise[1] - luma, 0.2F + noise[2] - luma};
    });
}

DevelopSettings noiseSettings(float luminance, float color, float detail = 50.0F,
                              float smoothness = 50.0F) {
    DevelopSettings settings;
    settings.noiseReduction.luminance = luminance;
    settings.noiseReduction.luminanceDetail = detail;
    settings.noiseReduction.color = color;
    settings.noiseReduction.colorSmoothness = smoothness;
    return settings;
}

/// @brief Luminance of a pixel through a plan's row.
float lumaAt(const ImageBuffer& image, const DenoisePlan& plan, std::size_t pixel) {
    const auto samples = image.samples<float>();
    const float* p = &samples[pixel * 4];
    return plan.lumaRow[0] * p[0] + plan.lumaRow[1] * p[1] + plan.lumaRow[2] * p[2];
}

/// @brief Variance of one channel over a rectangle of columns, all rows.
double varianceOf(const ImageBuffer& image, std::uint32_t firstColumn, std::uint32_t lastColumn,
                  std::size_t channel) {
    const auto samples = image.samples<float>();
    const ImageSize size = image.size();
    double sum = 0.0;
    double squares = 0.0;
    double count = 0.0;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = firstColumn; x <= lastColumn; ++x) {
            const double value =
                samples[(static_cast<std::size_t>(y) * size.width + x) * 4 + channel];
            sum += value;
            squares += value * value;
            count += 1.0;
        }
    }
    const double mean = sum / count;
    return squares / count - mean * mean;
}

/// @brief Variance of one channel over the pixel's luminance, over a rectangle of columns, all
/// rows.
double chromaVarianceOf(const ImageBuffer& image, const DenoisePlan& plan,
                        std::uint32_t firstColumn, std::uint32_t lastColumn, std::size_t channel) {
    const auto samples = image.samples<float>();
    const ImageSize size = image.size();
    double sum = 0.0;
    double squares = 0.0;
    double count = 0.0;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = firstColumn; x <= lastColumn; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * size.width + x;
            const double value = samples[pixel * 4 + channel] / lumaAt(image, plan, pixel);
            sum += value;
            squares += value * value;
            count += 1.0;
        }
    }
    const double mean = sum / count;
    return squares / count - mean * mean;
}

/// @brief Mean of one channel over one column.
double columnMean(const ImageBuffer& image, std::uint32_t column, std::size_t channel) {
    const auto samples = image.samples<float>();
    const ImageSize size = image.size();
    double sum = 0.0;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        sum += samples[(static_cast<std::size_t>(y) * size.width + column) * 4 + channel];
    }
    return sum / size.height;
}

/// @brief Whether two buffers hold the same bytes.
bool sameBytes(const ImageBuffer& first, const ImageBuffer& second) {
    return first.size() == second.size() && first.format() == second.format() &&
           std::ranges::equal(first.bytes(), second.bytes());
}

} // namespace

TEST_CASE("With every amount at zero noise reduction does not exist", "[denoise]") {
    // The other controls say how, not whether: with both amounts at zero the
    // block is the default, and so are the plan and the pixels.
    const ImageBuffer source = chromaNoise({24, 16}, 0.3F);
    DevelopSettings shaped = noiseSettings(0.0F, 0.0F, 10.0F, 80.0F);
    const DevelopSettings colourWithoutSmoothness = noiseSettings(0.0F, 60.0F, 50.0F, 0.0F);

    for (const DevelopSettings& settings : {shaped, colourWithoutSmoothness}) {
        const ProcessingPlan plan = planFor(source, DevelopState{settings});
        REQUIRE(plan.denoise == DenoisePlan{});
        REQUIRE_FALSE(plan.denoise.active());
        REQUIRE_FALSE(reducesNoise(settings.noiseReduction));
        REQUIRE(plan == planFor(source, DevelopState{}));
        REQUIRE(
            sameBytes(develop(source, DevelopState{settings}), develop(source, DevelopState{})));
    }

    SECTION("the front ends' predicate agrees with the plan") {
        for (const DevelopSettings& settings :
             {noiseSettings(1.0F, 0.0F), noiseSettings(0.0F, 1.0F, 50.0F, 1.0F),
              noiseSettings(0.0F, 1.0F, 50.0F, 0.0F), noiseSettings(0.0F, 0.0F, 0.0F, 100.0F),
              noiseSettings(250.0F, -5.0F)}) {
            REQUIRE(reducesNoise(settings.noiseReduction) ==
                    planFor(source, DevelopState{settings}).denoise.active());
        }
    }

    SECTION("a checkpoint at the boundary holds the source as it stands") {
        const ImageBuffer u16 = test::rainbow({9, 7}, PixelFormat::RgbaU16, workingEncoding);
        const RenderCheckpoint denoised = developUntil(u16, DevelopState{shaped}, Stage::Denoise);
        REQUIRE(denoised.boundary() == Stage::Denoise);
        const ImageBuffer held = denoised.readBack();
        REQUIRE(held.format() == PixelFormat::RgbaF32);
        REQUIRE(held.encoding() == u16.encoding());
        const auto heldSamples = held.samples<float>();
        const auto source16 = u16.samples<std::uint16_t>();
        for (std::size_t index = 0; index < heldSamples.size(); ++index) {
            REQUIRE(heldSamples[index] == static_cast<float>(source16[index]) / 65535.0F);
        }
    }
}

TEST_CASE("Colour noise reduction keeps every pixel's luminance", "[denoise]") {
    // Both ratios are unit-luma, so their blend is too, and recombining with
    // the pixel's own luminance gives it back (main's ADR 0034), whatever the
    // strength, also near black and below it.
    ImageBuffer source = chromaNoise({37, 23}, 0.8F);
    {
        // Dark and negative corners: the floored ratio must hold there too.
        const auto samples = source.samples<float>();
        samples[0] = -0.01F;
        samples[1] = 0.0F;
        samples[2] = 0.002F;
        samples[4] = 1e-7F;
        samples[5] = -1e-6F;
        samples[6] = 0.0F;
    }
    for (const float strength : {30.0F, 100.0F}) {
        const ProcessingPlan plan =
            planFor(source, DevelopState{noiseSettings(0.0F, strength, 50.0F, 60.0F)});
        REQUIRE(plan.denoise.color);
        REQUIRE_FALSE(plan.denoise.luminance);
        const ImageBuffer denoised = applyDenoise(source, plan.denoise);
        const auto pixels = static_cast<std::size_t>(source.size().pixelCount());
        double moved = 0.0;
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            const float before = lumaAt(source, plan.denoise, pixel);
            const float after = lumaAt(denoised, plan.denoise, pixel);
            REQUIRE(std::abs(after - before) <= 2e-6F * std::max(std::abs(before), 0.05F));
            moved =
                std::max(moved, static_cast<double>(std::abs(denoised.samples<float>()[pixel * 4] -
                                                             source.samples<float>()[pixel * 4])));
        }
        REQUIRE(moved > 1e-3); // and the colour did move
    }
}

TEST_CASE("Colour noise reduction smooths chroma noise on a flat patch", "[denoise]") {
    // Independent noise per channel is part chroma, part luminance; colour
    // smoothing removes the first and keeps the second, so what is measured is
    // the chroma: each channel over the pixel's luminance.
    const ImageBuffer source = chromaNoise({64, 48}, 0.5F);
    const ProcessingPlan plan =
        planFor(source, DevelopState{noiseSettings(0.0F, 100.0F, 50.0F, 50.0F)});
    const ImageBuffer denoised = applyDenoise(source, plan.denoise);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        INFO("channel " << channel);
        REQUIRE(chromaVarianceOf(denoised, plan.denoise, 8, 55, channel) <
                0.1 * chromaVarianceOf(source, plan.denoise, 8, 55, channel));
    }
}

TEST_CASE("The bilateral smooths a flat noisy patch and keeps a step edge", "[denoise]") {
    const ImageSize size{64, 40};
    const ImageBuffer source = noisyStep(size, 0.04F);
    const ProcessingPlan plan =
        planFor(source, DevelopState{noiseSettings(100.0F, 0.0F, 50.0F, 50.0F)});
    REQUIRE(plan.denoise.luminance);
    REQUIRE(plan.denoise.spatialRadius == 6);
    const ImageBuffer denoised = applyDenoise(source, plan.denoise);

    // Each half, away from the edge, loses most of its variance.
    REQUIRE(varianceOf(denoised, 2, 24, 1) < 0.3 * varianceOf(source, 2, 24, 1));
    REQUIRE(varianceOf(denoised, 39, 61, 1) < 0.3 * varianceOf(source, 39, 61, 1));
    // The columns either side of the step keep their levels: nothing leaks
    // across a difference far above the edge-stop.
    const std::uint32_t edge = size.width / 2;
    REQUIRE(std::abs(columnMean(denoised, edge - 1, 1) - 0.1) < 0.005);
    REQUIRE(std::abs(columnMean(denoised, edge, 1) - 0.6) < 0.01);
    // Grey stays grey: luminance smoothing keeps the ratio.
    const auto samples = denoised.samples<float>();
    for (std::size_t index = 0; index < samples.size(); index += 4) {
        REQUIRE(std::abs(samples[index] - samples[index + 1]) <= 1e-6F);
        REQUIRE(std::abs(samples[index + 2] - samples[index + 1]) <= 1e-6F);
    }

    SECTION("a higher Detail keeps more of the noise") {
        const ProcessingPlan tight =
            planFor(source, DevelopState{noiseSettings(100.0F, 0.0F, 100.0F, 50.0F)});
        const ImageBuffer kept = applyDenoise(source, tight.denoise);
        REQUIRE(varianceOf(kept, 2, 24, 1) > varianceOf(denoised, 2, 24, 1));
    }
}

TEST_CASE("White balance and exposure never reach the denoise group", "[denoise][plan]") {
    // The luma split uses the camera's as-shot row, not toWorking (ADR 007).
    const ImageBuffer source = loadImage(test::fixture(skewedFixture));
    REQUIRE(std::holds_alternative<CameraNative>(source.encoding()));
    const DevelopSettings base = noiseSettings(40.0F, 60.0F);
    DevelopSettings moved = base;
    moved.tone.exposure = 1.5F;
    moved.color.whiteBalance = WhiteBalanceMode::Custom;
    moved.color.temperature = 3500.0F;
    moved.color.tint = 20.0F;

    const ProcessingPlan first = planFor(source, DevelopState{base});
    const ProcessingPlan second = planFor(source, DevelopState{moved});
    REQUIRE(first.denoise.active());
    REQUIRE(first.denoise == second.denoise);
    REQUIRE(std::get<0>(stagesOf(first)) == std::get<0>(stagesOf(second)));
    REQUIRE(prefixMatches(first, second, Stage::Denoise));
    REQUIRE_FALSE(prefixMatches(first, second, Stage::Pointwise));

    // The checkpoint therefore survives both edits, and resuming equals a fresh render.
    const RenderCheckpoint denoised = developUntil(source, DevelopState{base}, Stage::Denoise);
    const RenderCheckpoint resumed =
        resumeFrom(denoised, source, DevelopState{moved}, Stage::Effects);
    REQUIRE(sameBytes(resumed.readBack(), develop(source, DevelopState{moved})));

    // The row is the camera's own, and a neutral of unit luminance has luminance one.
    const float neutralLuma = first.denoise.lumaRow[0] * first.denoise.neutral[0] +
                              first.denoise.lumaRow[1] * first.denoise.neutral[1] +
                              first.denoise.lumaRow[2] * first.denoise.neutral[2];
    REQUIRE(std::abs(neutralLuma - 1.0F) < 1e-6F);
    REQUIRE(first.denoise.lumaRow != DenoisePlan{}.lumaRow);
    REQUIRE(first.denoise.lumaRow != colorspaces::workingLuminance);
}

TEST_CASE("A reduced source divides the radii by its pixel scale", "[denoise][preview]") {
    const NoiseReductionSettings settings{.luminance = 50.0F,
                                          .luminanceDetail = 50.0F,
                                          .luminanceFilter = LuminanceNoiseFilter::Bilateral,
                                          .color = 50.0F,
                                          .colorSmoothness = 80.0F};
    const ColorEncoding encoding{workingEncoding};
    const DenoisePlan full = denoisePlanFor(settings, encoding, 1.0);
    const DenoisePlan half = denoisePlanFor(settings, encoding, 2.0);
    const DenoisePlan quarter = denoisePlanFor(settings, encoding, 4.0);
    const DenoisePlan eighth = denoisePlanFor(settings, encoding, 8.0);

    REQUIRE(full.spatialSigma == luminanceNoiseSpatialSigma);
    REQUIRE(half.spatialSigma == luminanceNoiseSpatialSigma / 2.0F);
    REQUIRE(quarter.spatialSigma == luminanceNoiseSpatialSigma / 4.0F);
    REQUIRE(full.spatialRadius == 6);
    REQUIRE(half.spatialRadius == 3);
    REQUIRE(quarter.spatialRadius == 2);
    REQUIRE(eighth.spatialRadius == 1);
    REQUIRE(full.rangeSigma == eighth.rangeSigma);

    // The colour grid stays four sensor pixels a side while the scale allows.
    REQUIRE(full.gridReduction == 4);
    REQUIRE(half.gridReduction == 2);
    REQUIRE(quarter.gridReduction == 1);
    REQUIRE(eighth.gridReduction == 1);
    const float sigma = 80.0F * colorNoiseSigmaPerSmoothness / 4.0F;
    REQUIRE(full.colorSigma == sigma);
    REQUIRE(half.colorSigma == sigma);
    REQUIRE(quarter.colorSigma == sigma);
    REQUIRE(eighth.colorSigma == sigma / 2.0F);

    // A scale that is not a power of two keeps whole source pixels per cell.
    const DenoisePlan third = denoisePlanFor(settings, encoding, 3.0);
    REQUIRE(third.gridReduction == 1);
    REQUIRE(third.spatialSigma == luminanceNoiseSpatialSigma / 3.0F);
    // A scale whose cell would be three rounds down to a power of two, so the
    // grid division stays exact in float on both backends.
    REQUIRE(denoisePlanFor(settings, encoding, 1.2).gridReduction == 2);
    // An enlarged source keeps the full-resolution cell and widens the blur.
    REQUIRE(denoisePlanFor(settings, encoding, 0.5).gridReduction == 4);
    for (const double bad : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
        REQUIRE_THROWS_AS(denoisePlanFor(settings, encoding, bad), std::invalid_argument);
    }

    // The pixels carry their scale into the plan, and only into the denoise group.
    DevelopSettings on;
    on.noiseReduction = settings;
    ImageBuffer source = chromaNoise({16, 12}, 0.2F);
    REQUIRE(planFor(source, DevelopState{on}).denoise == full);
    source.setPixelScale(4.0);
    REQUIRE(planFor(source, DevelopState{on}).denoise == quarter);
    REQUIRE(planFor(source, DevelopState{}) ==
            planFor(chromaNoise({16, 12}, 0.2F), DevelopState{}));
    // Two halvings make the same quarter, whoever asks for the render.
    const ImageBuffer level2 = halved(halved(chromaNoise({64, 48}, 0.2F)));
    REQUIRE(level2.pixelScale() == 4.0);
    REQUIRE(planFor(level2, DevelopState{on}).denoise == quarter);
    REQUIRE(planFor(level2, DevelopState{on}, {.size = RenderRequest::Scale{0.5}}).denoise ==
            quarter);

    SECTION("a level's colour smoothing approximates the full render's, reduced") {
        // The grid is the same in sensor pixels, so a reduced copy denoised at
        // its scale matches the full result reduced, up to the box filters'
        // differing order. The source has flat luminance, so what is compared
        // is the colour blur alone.
        const ImageBuffer large = flatLumaChromaNoise({128, 96}, 0.6F);
        DevelopSettings colour = noiseSettings(0.0F, 100.0F, 50.0F, 60.0F);
        const ImageBuffer fullThenHalved =
            halved(applyDenoise(large, planFor(large, DevelopState{colour}).denoise));
        const ImageBuffer reduced = halved(large);
        const ImageBuffer halvedThenDenoised =
            applyDenoise(reduced, planFor(reduced, DevelopState{colour}).denoise);
        // The rejected alternative: a grid that is always a quarter of the
        // level, so that the reduced copy blurs twice as far in sensor pixels.
        DenoisePlan quarterOfLevel = planFor(large, DevelopState{colour}).denoise;
        quarterOfLevel.gridReduction = colorNoiseGridReduction;
        const ImageBuffer fixedQuarter = applyDenoise(reduced, quarterOfLevel);
        const auto worstAgainst = [&](const ImageBuffer& other) {
            const auto a = fullThenHalved.samples<float>();
            const auto b = other.samples<float>();
            double worst = 0.0;
            for (std::size_t index = 0; index < a.size(); ++index) {
                worst = std::max(worst, static_cast<double>(std::abs(a[index] - b[index])));
            }
            return worst;
        };
        const double sensorGrid = worstAgainst(halvedThenDenoised);
        const double levelGrid = worstAgainst(fixedQuarter);
        CAPTURE(sensorGrid, levelGrid);
        REQUIRE(sensorGrid < 0.01);
        REQUIRE(levelGrid > 2.0 * sensorGrid);
    }
}

TEST_CASE("A half-size decode develops with half the reach", "[denoise][preview][raw]") {
    // The thumbnail's path: a half-size RAW decode, then halvings to its level.
    const ImageBuffer full = loadImage(test::fixture("bayer-32x24.dng"));
    const ImageBuffer halfSize =
        loadImage(test::fixture("bayer-32x24.dng"), discardedDiagnostics(), {.halfSize = true});
    REQUIRE(full.pixelScale() == 1.0);
    REQUIRE(halfSize.size() == ImageSize{16, 12});
    REQUIRE(halfSize.pixelScale() == 2.0);
    const DevelopState state{noiseSettings(50.0F, 50.0F)};
    const DenoisePlan plan = planFor(halfSize, state).denoise;
    REQUIRE(plan.spatialSigma == luminanceNoiseSpatialSigma / 2.0F);
    REQUIRE(plan.spatialRadius == 3);
    REQUIRE(plan.gridReduction == 2);
    const ImageBuffer level1 = halved(halfSize);
    REQUIRE(level1.pixelScale() == 4.0);
    REQUIRE(planFor(level1, state).denoise.spatialRadius == 2);
    REQUIRE(planFor(level1, state).denoise.gridReduction == 1);
    // Developing keeps the scale up to the resize, which multiplies it by the reduction.
    REQUIRE(develop(halfSize, state).pixelScale() == 2.0);
    REQUIRE(develop(halfSize, state, {.size = RenderRequest::Scale{0.5}}).pixelScale() == 4.0);
}

TEST_CASE("A region render is the crop of the whole render", "[denoise][region]") {
    // The region is cut after geometry and noise reduction sees the whole
    // source, so a region needs no margin of its own (ADR 025, ADR 039).
    const ImageBuffer source = chromaNoise({40, 30}, 0.5F);
    const DevelopState state{noiseSettings(70.0F, 80.0F, 40.0F, 30.0F)};
    const ImageBuffer whole = develop(source, state);
    RenderRequest request;
    request.region = RenderRequest::Region{.left = 0.25, .top = 0.2, .right = 0.75, .bottom = 0.7};
    const ImageBuffer part = develop(source, state, request);
    REQUIRE(part.size() == ImageSize{20, 15});
    const auto in = whole.samples<float>();
    const auto out = part.samples<float>();
    for (std::uint32_t y = 0; y < 15; ++y) {
        for (std::uint32_t x = 0; x < 20; ++x) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
                REQUIRE(out[(static_cast<std::size_t>(y) * 20 + x) * 4 + channel] ==
                        in[(static_cast<std::size_t>(y + 6) * 40 + x + 10) * 4 + channel]);
            }
        }
    }
    // The same on a reduced source, whose scale the region keeps.
    ImageBuffer reduced = source.clone();
    reduced.setPixelScale(2.0);
    const ImageBuffer reducedWhole = develop(reduced, state);
    const ImageBuffer reducedPart = develop(reduced, state, request);
    REQUIRE(reducedPart.pixelScale() == 2.0);
    const auto reducedIn = reducedWhole.samples<float>();
    const auto reducedOut = reducedPart.samples<float>();
    for (std::uint32_t y = 0; y < 15; ++y) {
        for (std::uint32_t x = 0; x < 20; ++x) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
                REQUIRE(reducedOut[(static_cast<std::size_t>(y) * 20 + x) * 4 + channel] ==
                        reducedIn[(static_cast<std::size_t>(y + 6) * 40 + x + 10) * 4 + channel]);
            }
        }
    }
    // What a render restricted to the region would need around it.
    const ProcessingPlan plan = planFor(source, state);
    REQUIRE(denoiseReach(plan.denoise) ==
            std::max(plan.denoise.spatialRadius,
                     (plan.denoise.colorRadius + 2) * plan.denoise.gridReduction));
    REQUIRE(denoiseReach(DenoisePlan{}) == 0);
}

TEST_CASE("Renders stop and resume at the denoise boundary", "[denoise][resume]") {
    const ImageBuffer source = chromaNoise({24, 18}, 0.4F);
    const DevelopState state{noiseSettings(50.0F, 60.0F)};
    const ImageBuffer direct = develop(source, state);

    const RenderCheckpoint denoised = developUntil(source, state, Stage::Denoise);
    REQUIRE(denoised.boundary() == Stage::Denoise);
    REQUIRE(denoised.size() == source.size());
    REQUIRE(denoised.encoding() == source.encoding());
    REQUIRE(sameBytes(denoised.readBack(), applyDenoise(source, planFor(source, state).denoise)));
    REQUIRE(sameBytes(resumeFrom(denoised, source, state, Stage::Effects).readBack(), direct));
    REQUIRE(sameBytes(developUntil(source, state, Stage::Effects).readBack(), direct));

    // A tone edit keeps it; a noise reduction edit or another level does not.
    DevelopSettings toned = state.settings;
    toned.tone.exposure = 0.7F;
    REQUIRE(sameBytes(resumeFrom(denoised, source, DevelopState{toned}, Stage::Effects).readBack(),
                      develop(source, DevelopState{toned})));
    DevelopSettings stronger = state.settings;
    stronger.noiseReduction.color = 90.0F;
    REQUIRE_THROWS_AS(resumeFrom(denoised, source, DevelopState{stronger}, Stage::Pointwise),
                      std::invalid_argument);
    // Pixels of another scale make another denoise group.
    ImageBuffer otherScale = source.clone();
    otherScale.setPixelScale(2.0);
    REQUIRE_THROWS_AS(resumeFrom(denoised, otherScale, state, Stage::Pointwise),
                      std::invalid_argument);

    // Stopping earlier than a checkpoint is refused, as at every boundary.
    const RenderCheckpoint pointwise = developUntil(source, state, Stage::Pointwise);
    REQUIRE_THROWS_AS(resumeFrom(pointwise, source, state, Stage::Denoise), std::invalid_argument);
    // A denoise checkpoint resumed at its own boundary is itself.
    REQUIRE(resumeFrom(denoised, source, state, Stage::Denoise).boundary() == Stage::Denoise);
    // Another source size is refused by the size check.
    const ImageBuffer other = chromaNoise({20, 18}, 0.4F);
    REQUIRE_THROWS_AS(resumeFrom(denoised, other, state, Stage::Pointwise), std::invalid_argument);
}

TEST_CASE("A sample at the curve input sees the denoised source", "[denoise][sample]") {
    const ImageBuffer source = chromaNoise({16, 12}, 0.5F);
    const DevelopState off{};
    const DevelopState on{noiseSettings(80.0F, 80.0F)};
    REQUIRE_FALSE(
        sameBytes(sample(source, off, Tap::CurveInput), sample(source, on, Tap::CurveInput)));
    REQUIRE_FALSE(sameAtTap(planFor(source, off), planFor(source, on), Tap::CurveInput));
}

TEST_CASE("Noise reduction refuses what it cannot resolve", "[denoise]") {
    NoiseReductionSettings settings;
    settings.luminance = std::nanf("");
    REQUIRE_THROWS_AS(denoisePlanFor(settings, workingEncoding, 1.0), std::invalid_argument);
    settings = {};
    settings.luminanceFilter = static_cast<LuminanceNoiseFilter>(9);
    REQUIRE_THROWS_AS(denoisePlanFor(settings, workingEncoding, 1.0), std::invalid_argument);
    settings = {};
    settings.color = 250.0F; // clamped, as every setting the maths reads
    REQUIRE(denoisePlanFor(settings, workingEncoding, 1.0).colorMix == 1.0F);
}

TEST_CASE("The threaded CPU pass gives the single-threaded bits", "[denoise][threads]") {
    // Large enough for several bands, odd so the bands are uneven, with the
    // radius reaching across band edges.
    const ImageBuffer source = chromaNoise({517, 509}, 0.5F);
    const DenoisePlan plan = planFor(source, DevelopState{noiseSettings(60.0F, 70.0F)}).denoise;
    // Set, so that the test splits even on a machine of one thread.
    detail::rowBandLimit = 4;
    REQUIRE(detail::rowBandCount(source.size().pixelCount()) == 4);
    const ImageBuffer threaded = applyDenoise(source, plan);
    detail::rowBandLimit = 1;
    const ImageBuffer single = applyDenoise(source, plan);
    detail::rowBandLimit = 3;
    const ImageBuffer three = applyDenoise(source, plan);
    detail::rowBandLimit = 0;
    REQUIRE(sameBytes(threaded, single));
    REQUIRE(sameBytes(three, single));
}

TEST_CASE("Row bands cover every row once and pass on a failure", "[denoise][threads]") {
    std::vector<int> seen(1000, 0);
    detail::forEachRowBand(1000, 1000, [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t row = first; row < last; ++row) {
            ++seen[row];
        }
    });
    REQUIRE(std::ranges::all_of(seen, [](int count) { return count == 1; }));
    detail::forEachRowBand(0, 10, [](std::uint32_t, std::uint32_t) { FAIL("no rows, no call"); });
    REQUIRE_THROWS_AS(detail::forEachRowBand(1000, 1000,
                                             [](std::uint32_t first, std::uint32_t) {
                                                 if (first > 0) {
                                                     throw std::runtime_error("band failed");
                                                 }
                                             }),
                      std::runtime_error);
}
