#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuPlan.h"
#include "GpuTesting.h"
#include "LocalPlan.h"
#include "PointwisePlan.h"
#include "ProcessingPlan.h"
#include "SampleConversion.h"
#include "support/LocalAdjustmentStates.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImagePyramid.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <RenderCheckpoint.h>
#include <SettingDescriptors.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// Local adjustments on the device against the CPU (ADR 044, section 7).
///
/// Every case renders the same state on both backends and holds the pointwise boundary, or a
/// whole render, to the tolerances below. Each case also checks that its masks changed the
/// picture, so that a bound cannot be met by a render in which nothing happened.

namespace {

/// @brief Largest error of a pointwise pass under masks against the CPU, relative to the pixel's
/// scale.
///
/// The shader's own arithmetic is the weights (a few float operations and a smoothstep, whose
/// error is a few ulp of the weight), the sums, and the resolutions of ADR 044, section 2:
/// `exp2`, `pow` and divisions, whose driver precision is the one the pointwise bound already
/// allows. An error of a few ulp of a weight is an error of the same relative size in a delta of
/// at most 100, which the chain turns into a relative error of the same order as the weight's,
/// so the bound is the pointwise one. Measured worst on lavapipe over every case below: see the
/// printout (ARRAW_PRINT_MEASURED). A wrong weight, sum, clamp or gate disagrees by 1e-3 or more.
constexpr double localRelativeTolerance = pointwiseRelativeTolerance;

/// @brief Largest error of a whole render (pointwise, geometry and resize) under masks.
constexpr double localEndToEndTolerance = pointwiseRelativeTolerance + resampleTolerance;

/// @brief Whether the measured errors are printed.
bool printsMeasured() {
    return std::getenv("ARRAW_PRINT_MEASURED") != nullptr;
}

/// @brief A tinted scene of smooth shapes and fine detail, in the working encoding.
ImageBuffer sceneOf(ImageSize size) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double shapes = 0.6 * std::sin(x / 5.0) * std::cos(y / 4.0) +
                                  0.3 * std::sin((x + y) / 11.0) +
                                  0.08 * std::sin(x / 1.3) * std::sin(y / 1.7);
            // Four stops either way, so that blacks and whites have pixels to act on.
            const auto value = static_cast<float>(0.2 * std::exp2(4.0 * shapes));
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = 1.15F * value;
            pixel[1] = value;
            pixel[2] = 0.75F * value;
            pixel[3] = 1.0F;
        }
    }
    return image;
}

/// @brief A flat grey image.
ImageBuffer greyOf(ImageSize size, float level) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    for (float& sample : image.samples<float>()) {
        sample = level;
    }
    for (std::size_t pixel = 3; pixel < image.samples<float>().size(); pixel += 4) {
        image.samples<float>()[pixel] = 1.0F;
    }
    return image;
}

/// @brief Builds an adjustment of a shape with some deltas.
LocalAdjustment adjustmentOf(Mask shape, const std::vector<std::pair<std::string, double>>& deltas,
                             float opacity = 1.0F, bool invert = false) {
    LocalAdjustment adjustment;
    adjustment.shape = std::move(shape);
    adjustment.opacity = opacity;
    adjustment.invert = invert;
    for (const auto& [key, value] : deltas) {
        const LocalDescriptor* descriptor = findLocalDescriptor(key);
        REQUIRE(descriptor != nullptr);
        adjustment.deltas.*descriptor->member = static_cast<float>(value);
    }
    return adjustment;
}

/// @brief Builds a state from adjustments, in order.
DevelopState stateOf(const std::vector<LocalAdjustment>& adjustments,
                     DevelopSettings settings = {}) {
    DevelopState state{settings};
    for (const LocalAdjustment& adjustment : adjustments) {
        state = withLocalAdjustmentAdded(std::move(state), adjustment);
    }
    return state;
}

/// @brief The same state without its masks.
DevelopState withoutMasks(DevelopState state) {
    state.localAdjustments.clear();
    return state;
}

/// @brief A linear mask that is one on the left of a frame and zero on the right.
LinearMask leftToRight(float from = 0.15F, float to = 0.85F) {
    return {.from = {from, 0.5F}, .to = {to, 0.5F}};
}

/// @brief A linear mask that is one everywhere in the frame.
LinearMask everywhere() {
    return {.from = {0.5F, 2.0F}, .to = {0.5F, 2.5F}};
}

/// @brief Requires the device's pointwise boundary to match the CPU's under the state's masks.
/// @return The worst error, relative to the pixel's scale.
double requirePointwiseMatches(const ImageBuffer& source, const DevelopState& state,
                               double tolerance = localRelativeTolerance) {
    REQUIRE_FALSE(state.localAdjustments.empty());
    const ImageBuffer expected = developUntil(source, state, Stage::Pointwise).readBack();
    const ImageBuffer unmasked =
        developUntil(source, withoutMasks(state), Stage::Pointwise).readBack();
    const ImageBuffer actual =
        developOnGpu(gpuContext(), source, state, Stage::Pointwise).readBack();
    REQUIRE(actual.size() == expected.size());
    const double effect = worstColourError(unmasked, expected);
    const double error = worstColourError(expected, actual);
    if (printsMeasured()) {
        std::fprintf(stderr, "local pointwise: effect %.3g, error %.3g\n", effect, error);
    }
    CAPTURE(effect, error, tolerance, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    // The masks did something, a hundred times more than the bound.
    REQUIRE(effect > 1e-3);
    REQUIRE(error <= tolerance);
    return error;
}

/// @brief Requires two images to have the same bits.
void requireBitIdentical(const ImageBuffer& expected, const ImageBuffer& actual) {
    REQUIRE(actual.size() == expected.size());
    const FloatDifference difference = compareFloat(expected, actual, pointwiseAbsoluteFloor);
    CAPTURE(difference);
    REQUIRE(difference.bitExact);
}

/// @brief A delta that does something visible for a control.
double visibleDelta(const LocalDescriptor& descriptor) {
    return descriptor.range.maximum * 0.6;
}

} // namespace

TEST_CASE("Each local control alone, in a linear and a radial mask, matches the CPU",
          "[gpu][local]") {
    // An odd size, so that the last row and column are partial.
    const ImageBuffer source = sceneOf({131, 77});
    double worst = 0.0;
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        for (const double sign : {1.0, -1.0}) {
            INFO(descriptor.key << ", sign " << sign);
            const double delta = sign * visibleDelta(descriptor);
            const DevelopState linear =
                stateOf({adjustmentOf(leftToRight(), {{std::string(descriptor.key), delta}})});
            const DevelopState radial = stateOf({adjustmentOf(
                someRadial(), {{std::string(descriptor.key), delta}}, 0.8F, sign < 0.0)});
            worst = std::max(worst, requirePointwiseMatches(source, linear));
            worst = std::max(worst, requirePointwiseMatches(source, radial));
        }
    }
    if (printsMeasured()) {
        std::fprintf(stderr, "local controls: worst %.3g\n", worst);
    }
}

TEST_CASE("Every control together, with the globals they add to, matches the CPU", "[gpu][local]") {
    const ImageBuffer source = sceneOf({131, 77});
    DevelopSettings settings;
    settings.tone = {0.3F, 20.0F, 15.0F, -20.0F, 10.0F, -10.0F, 0.0F};
    settings.color.saturation = 20.0F;
    settings.color.vibrance = -10.0F;
    settings.presence = {.texture = 20.0F, .clarity = -15.0F, .dehaze = 25.0F};
    DevelopState state =
        stateOf({adjustmentOf(someLinear(), {}), adjustmentOf(someRadial(), {})}, settings);
    LocalDeltas first;
    LocalDeltas second;
    giveEveryDelta(first);
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        second.*descriptor.member = -0.5F * first.*descriptor.member;
    }
    state.localAdjustments[0].deltas = first;
    state.localAdjustments[1].deltas = second;
    state.localAdjustments[1].invert = true;
    requirePointwiseMatches(source, state);
}

TEST_CASE("A mask is anchored at pixel centres, at odd and non-square sizes", "[gpu][local]") {
    for (const ImageSize size : {ImageSize{1, 1}, ImageSize{2, 3}, ImageSize{17, 5},
                                 ImageSize{5, 17}, ImageSize{97, 61}, ImageSize{61, 97}}) {
        INFO(size.width << "x" << size.height);
        const ImageBuffer source = sceneOf(size);
        requirePointwiseMatches(
            source, stateOf({adjustmentOf(someLinear(), {{"exposure", 1.5}, {"blacks", 40.0}}),
                             adjustmentOf(someRadial(), {{"shadows", 50.0}})}));
    }
}

TEST_CASE("A narrow feather matches the CPU", "[gpu][local]") {
    // A feather of f radii is f * r pixels wide: at 0 it is the one pixel the plan floors it to
    // (ADR 044, section 4). The distance is rounded by the device and the host to within a few
    // ulp, which moves a weight by that much over the feather's width; the frames below are
    // wide enough for a feather of a pixel or so, where that is still far below the bound.
    // Measured worst on lavapipe: see the printout (ARRAW_PRINT_MEASURED).
    for (const ImageSize size : {ImageSize{203, 77}, ImageSize{1203, 53}}) {
        const ImageBuffer source = sceneOf(size);
        for (const float feather : {0.0F, 0.001F, 0.01F, 0.05F, 0.2F}) {
            INFO(size.width << "x" << size.height << ", feather " << feather);
            RadialMask mask = someRadial();
            mask.feather = feather;
            requirePointwiseMatches(source, stateOf({adjustmentOf(mask, {{"exposure", 1.0}})}));
            requirePointwiseMatches(source,
                                    stateOf({adjustmentOf(mask, {{"exposure", 1.0}}, 1.0F, true)}));
        }
    }
}

TEST_CASE("Sixteen overlapping masks match the CPU, and the sum written as one", "[gpu][local]") {
    const ImageBuffer source = sceneOf({131, 77});
    std::vector<LocalAdjustment> adjustments;
    LocalDeltas total;
    for (int index = 0; index < 16; ++index) {
        const float across = 0.2F + 0.04F * static_cast<float>(index);
        LocalDeltas deltas;
        deltas.exposure = 0.1F + 0.05F * static_cast<float>(index % 5);
        deltas.shadows = (index % 2 == 0 ? 6.0F : -2.0F);
        deltas.dehaze = static_cast<float>(index % 3) - 1.0F;
        deltas.relativeTemperature = (index % 4 == 0) ? 4.0F : -1.0F;
        deltas.saturation = 2.0F;
        for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
            total.*descriptor.member += deltas.*descriptor.member;
        }
        LocalAdjustment adjustment;
        adjustment.deltas = deltas;
        adjustment.invert = index % 7 == 3;
        adjustment.opacity = index % 3 == 0 ? 0.7F : 1.0F;
        if (index % 2 == 0) {
            adjustment.shape = LinearMask{.from = {across, 0.2F}, .to = {1.0F - across, 0.9F}};
        } else {
            adjustment.shape = RadialMask{.centre = {across, 0.5F},
                                          .radiusX = 0.4F,
                                          .radiusY = 0.3F,
                                          .angle = 11.0F * static_cast<float>(index),
                                          .feather = 0.6F};
        }
        adjustments.push_back(adjustment);
    }
    const DevelopState state = stateOf(adjustments);
    REQUIRE(state.localAdjustments.size() == 16);
    const ProcessingPlan plan = planFor(source, state);
    REQUIRE(plan.pointwise.local.masks.size() == 16);
    requirePointwiseMatches(source, state);
}

TEST_CASE("Masks that cancel exactly leave the global render, bit for bit", "[gpu][local]") {
    const ImageBuffer source = sceneOf({131, 77});
    DevelopSettings settings;
    settings.tone.exposure = 0.3F;
    settings.tone.contrast = 10.0F;
    settings.presence.clarity = 30.0F;
    // Equal weights and opposite deltas, in every control, for linear and for radial masks.
    LocalDeltas deltas;
    giveEveryDelta(deltas);
    LocalDeltas opposite;
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        opposite.*descriptor.member = -(deltas.*descriptor.member);
    }
    for (const Mask& shape : {Mask{someLinear()}, Mask{someRadial()}}) {
        LocalAdjustment up;
        up.shape = shape;
        up.deltas = deltas;
        LocalAdjustment down = up;
        down.deltas = opposite;
        const DevelopState state = stateOf({up, down}, settings);
        const ImageBuffer without =
            developOnGpu(gpuContext(), source, withoutMasks(state), Stage::Pointwise).readBack();
        const ImageBuffer masked =
            developOnGpu(gpuContext(), source, state, Stage::Pointwise).readBack();
        // The plan's bases follow the reach, not the sums, so the Presence context may differ;
        // cancellation is exact in the chain's amounts, so the pixels are the global ones.
        requireBitIdentical(without, masked);
    }
}

TEST_CASE("Pixels no mask reaches are the unmasked render, bit for bit", "[gpu][local]") {
    // A radial mask on the left of the frame; its right half has weight exactly zero.
    const ImageBuffer source = sceneOf({131, 77});
    RadialMask mask{.centre = {0.2F, 0.5F}, .radiusX = 0.15F, .radiusY = 0.3F, .feather = 0.3F};
    DevelopSettings settings;
    settings.tone.exposure = 0.2F;
    settings.color.saturation = 10.0F;
    const DevelopState state =
        stateOf({adjustmentOf(mask, {{"exposure", 1.0}, {"saturation", 30.0}, {"blacks", 20.0}})},
                settings);
    const ImageBuffer without =
        developOnGpu(gpuContext(), source, withoutMasks(state), Stage::Pointwise).readBack();
    const ImageBuffer masked =
        developOnGpu(gpuContext(), source, state, Stage::Pointwise).readBack();
    const std::span<const float> want = without.samples<float>();
    const std::span<const float> got = masked.samples<float>();
    std::size_t outside = 0;
    std::size_t inside = 0;
    for (std::uint32_t y = 0; y < source.size().height; ++y) {
        for (std::uint32_t x = 0; x < source.size().width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * source.size().width + x) * 4;
            const bool same =
                std::bit_cast<std::uint32_t>(want[at]) == std::bit_cast<std::uint32_t>(got[at]) &&
                std::bit_cast<std::uint32_t>(want[at + 1]) ==
                    std::bit_cast<std::uint32_t>(got[at + 1]) &&
                std::bit_cast<std::uint32_t>(want[at + 2]) ==
                    std::bit_cast<std::uint32_t>(got[at + 2]);
            if (x > source.size().width / 2) {
                CAPTURE(x, y);
                REQUIRE(same);
                ++outside;
            } else if (!same) {
                ++inside;
            }
        }
    }
    REQUIRE(outside > 1000);
    REQUIRE(inside > 100);
}

TEST_CASE("Masks that do nothing leave the render as the unmasked one", "[gpu][local]") {
    const ImageBuffer source = sceneOf({61, 41});
    DevelopState state =
        stateOf({adjustmentOf(someLinear(), {{"exposure", 1.0}}, 0.0F),
                 adjustmentOf(someRadial(), {{"exposure", 1.0}}), adjustmentOf(someRadial(), {})});
    state = withLocalAdjustmentEnabled(state, state.localAdjustments[1].id, false);
    const ImageBuffer without =
        developOnGpu(gpuContext(), source, withoutMasks(state), Stage::Pointwise).readBack();
    requireBitIdentical(without,
                        developOnGpu(gpuContext(), source, state, Stage::Pointwise).readBack());
}

TEST_CASE("Local Temperature and Tint match the CPU, and the table of ADR 044", "[gpu][local]") {
    const ImageBuffer source = sceneOf({131, 77});
    requirePointwiseMatches(source,
                            stateOf({adjustmentOf(someLinear(), {{"relativeTemperature", 70.0}}),
                                     adjustmentOf(someRadial(), {{"relativeTint", -60.0}})}));
    requirePointwiseMatches(
        source, stateOf({adjustmentOf(everywhere(), {{"relativeTemperature", 50.0}}),
                         adjustmentOf(someRadial(),
                                      {{"relativeTemperature", 50.0}, {"relativeTint", 100.0}})}));

    SECTION("a neutral takes the gains of the table, and keeps its luminance") {
        struct Row {
            double temperature;
            double tint;
            std::array<float, 3> gains;
        };
        const Row rows[]{{100.0, 0.0, {1.100F, 0.985F, 0.731F}},
                         {-100.0, 0.0, {0.901F, 1.007F, 1.357F}},
                         {0.0, 100.0, {1.085F, 0.951F, 1.179F}},
                         {0.0, -100.0, {0.917F, 1.046F, 0.844F}}};
        const ImageBuffer grey = greyOf({4, 4}, 0.5F);
        for (const Row& row : rows) {
            INFO(row.temperature << ", " << row.tint);
            const DevelopState state =
                stateOf({adjustmentOf(everywhere(), {{"relativeTemperature", row.temperature},
                                                     {"relativeTint", row.tint}})});
            const ImageBuffer actual =
                developOnGpu(gpuContext(), grey, state, Stage::Pointwise).readBack();
            const std::span<const float> pixels = actual.samples<float>();
            for (std::size_t pixel = 0; pixel < 16; ++pixel) {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    REQUIRE(std::abs(pixels[pixel * 4 + channel] / 0.5F - row.gains[channel]) <
                            1e-3F);
                }
                const float luminance = 0.2627F * pixels[pixel * 4] +
                                        0.6780F * pixels[pixel * 4 + 1] +
                                        0.0593F * pixels[pixel * 4 + 2];
                REQUIRE(std::abs(luminance - 0.5F) < 1e-6F);
            }
        }
    }

    SECTION("two masks of Temperature 50 on full weight equal one of 100") {
        const DevelopState two =
            stateOf({adjustmentOf(everywhere(), {{"relativeTemperature", 50.0}}),
                     adjustmentOf(everywhere(), {{"relativeTemperature", 50.0}})});
        const DevelopState one =
            stateOf({adjustmentOf(everywhere(), {{"relativeTemperature", 100.0}})});
        const ImageBuffer a = developOnGpu(gpuContext(), source, two, Stage::Pointwise).readBack();
        const ImageBuffer b = developOnGpu(gpuContext(), source, one, Stage::Pointwise).readBack();
        REQUIRE(worstColourError(a, b) <= localRelativeTolerance);
    }
}

TEST_CASE("The after-matrix probe applies the local Temperature and Tint", "[gpu][local]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = sceneOf({61, 41});
    const DevelopState state = stateOf(
        {adjustmentOf(someLinear(), {{"relativeTemperature", 80.0}, {"relativeTint", 30.0}}),
         adjustmentOf(someRadial(), {{"relativeTint", -50.0}})});
    const ProcessingPlan plan = planFor(source, state);
    REQUIRE_FALSE(plan.pointwise.local.empty());

    const DeviceImage input = context.upload(source);
    const GpuPointwiseBlock block =
        packPointwise(plan.pointwise, source.size(), PointwiseProbe::AfterMatrix);
    // The image stands in for the Presence grids and the brush coverage, which are not read.
    const std::array inputs{input, input, input, input, input, input,
                            input, input, input, input, input};
    const ImageBuffer actual = context
                                   .render(GpuPass::Pointwise, std::as_bytes(std::span(&block, 1)),
                                           inputs, source.size(), workingEncoding)
                                   .readBack();
    ImageBuffer expected(source.size(), PixelFormat::RgbaF32, workingEncoding);
    const std::span<const float> in = source.samples<float>();
    const std::span<float> out = expected.samples<float>();
    for (std::uint32_t y = 0; y < source.size().height; ++y) {
        for (std::uint32_t x = 0; x < source.size().width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * source.size().width + x) * 4;
            const PixelAmounts amounts = amountsAt(plan.pointwise, x, y, PixelCoverage{});
            Colour colour = plan.pointwise.toWorking * Colour{in[at], in[at + 1], in[at + 2]};
            if (amounts.balances) {
                colour = {colour[0] * amounts.balance[0], colour[1] * amounts.balance[1],
                          colour[2] * amounts.balance[2]};
            }
            out[at] = colour[0];
            out[at + 1] = colour[1];
            out[at + 2] = colour[2];
            out[at + 3] = in[at + 3];
        }
    }
    REQUIRE(worstColourError(expected, actual) <= localRelativeTolerance);
    // And the probe differs from the unmasked matrix, so the gain was in it.
    ImageBuffer plain = expected.clone();
    for (std::size_t at = 0; at < in.size(); at += 4) {
        const Colour colour = plan.pointwise.toWorking * Colour{in[at], in[at + 1], in[at + 2]};
        plain.samples<float>()[at] = colour[0];
        plain.samples<float>()[at + 1] = colour[1];
        plain.samples<float>()[at + 2] = colour[2];
    }
    REQUIRE(worstColourError(plain, actual) > 1e-3);
}

TEST_CASE("Presence under masks matches the CPU", "[gpu][local][presence]") {
    const ImageBuffer source = sceneOf({203, 77});

    SECTION("mixed-sign Dehaze across one frame, from a global of zero") {
        requirePointwiseMatches(
            source, stateOf({adjustmentOf(leftToRight(0.1F, 0.55F), {{"dehaze", 80.0}}),
                             adjustmentOf(leftToRight(0.9F, 0.45F), {{"dehaze", -80.0}})}));
    }
    SECTION("mixed-sign Dehaze with overlap and tone") {
        DevelopSettings settings;
        settings.tone.exposure = 0.3F;
        requirePointwiseMatches(
            source, stateOf({adjustmentOf(someRadial(), {{"dehaze", 90.0}, {"exposure", 0.4}}),
                             adjustmentOf(someLinear(), {{"dehaze", -60.0}})},
                            settings));
    }
    SECTION("local-only Texture, Clarity and Dehaze, with the globals at zero") {
        for (const char* key : {"texture", "clarity", "dehaze"}) {
            for (const double delta : {70.0, -70.0}) {
                INFO(key << " " << delta);
                requirePointwiseMatches(source,
                                        stateOf({adjustmentOf(someRadial(), {{key, delta}})}));
            }
        }
    }
    SECTION("a global positive Dehaze under a negative mask") {
        DevelopSettings settings;
        settings.presence.dehaze = 50.0F;
        const DevelopState state =
            stateOf({adjustmentOf(someRadial(), {{"dehaze", -90.0}})}, settings);
        const ProcessingPlan plan = planFor(source, state);
        REQUIRE(plan.pointwise.presence.hazeFloor.active());
        REQUIRE(plan.pointwise.presence.hazeMean.active());
        requirePointwiseMatches(source, state);
    }
    SECTION("masks that cancel to zero read neither base") {
        DevelopSettings settings;
        settings.presence.dehaze = 40.0F;
        LocalAdjustment up = adjustmentOf(someRadial(), {{"dehaze", 60.0}});
        LocalAdjustment down = adjustmentOf(someRadial(), {{"dehaze", -60.0}});
        const DevelopState state = stateOf({up, down}, settings);
        const ImageBuffer without =
            developOnGpu(gpuContext(), source, withoutMasks(state), Stage::Pointwise).readBack();
        const ImageBuffer masked =
            developOnGpu(gpuContext(), source, state, Stage::Pointwise).readBack();
        requireBitIdentical(without, masked);
    }
    SECTION("an interval that ends exactly at zero prepares and reads no floor") {
        // Global -50 and a mask of +50 at full weight: the effective amount is zero at the
        // centre, and no floor exists anywhere.
        DevelopSettings settings;
        settings.presence.dehaze = -50.0F;
        const DevelopState state =
            stateOf({adjustmentOf(everywhere(), {{"dehaze", 50.0}})}, settings);
        const ProcessingPlan plan = planFor(source, state);
        REQUIRE_FALSE(plan.pointwise.presence.hazeFloor.active());
        REQUIRE(plan.pointwise.presence.hazeMean.active());
        requirePointwiseMatches(source, state);
    }
    SECTION("Texture and Clarity under a mask on a camera-sized grid") {
        const ImageBuffer wide = sceneOf({1203, 33});
        requirePointwiseMatches(
            wide, stateOf({adjustmentOf(
                      leftToRight(), {{"texture", 60.0}, {"clarity", 70.0}, {"dehaze", -50.0}})}));
    }
}

TEST_CASE("A render with masks matches the CPU at a reduced level, in a region and through the "
          "geometry",
          "[gpu][local][pyramid]") {
    GpuContext& context = gpuContext();
    DevelopSettings settings;
    settings.tone.exposure = 0.2F;
    settings.geometry.straighten = 4.0;
    settings.geometry.crop.rectangle = UprightCropRect{0.05, 0.1, 0.95, 0.9};
    const DevelopState state = stateOf(
        {adjustmentOf(someLinear(), {{"exposure", 0.8}, {"dehaze", 40.0}, {"saturation", 30.0}}),
         adjustmentOf(someRadial(), {{"relativeTemperature", 60.0}, {"contrast", -30.0}}, 0.9F,
                      true)},
        settings);
    const ImageBuffer full = sceneOf({203, 141});

    const auto requireRender = [&](const ImageBuffer& source, const RenderRequest& request) {
        const ImageBuffer expected = develop(source, state, request);
        const ImageBuffer unmasked = develop(source, withoutMasks(state), request);
        const ImageBuffer actual =
            developOnGpu(context, source, state, Stage::Effects, request).readBack();
        REQUIRE(actual.size() == expected.size());
        const double colour = worstColourError(expected, actual);
        if (printsMeasured()) {
            std::fprintf(stderr, "local end to end: %.3g\n", colour);
        }
        CAPTURE(colour, compareFloat(expected, actual, pointwiseAbsoluteFloor));
        REQUIRE(worstColourError(unmasked, expected) > 1e-3);
        REQUIRE(colour <= localEndToEndTolerance);
    };

    SECTION("the whole frame") {
        requireRender(full, {});
    }
    SECTION("fit inside") {
        requireRender(full, {.size = RenderRequest::FitInside{80, 80}});
    }
    SECTION("a region") {
        requireRender(full, {.size = RenderRequest::FitInside{60, 60},
                             .region = RenderRequest::Region{0.2, 0.1, 0.7, 0.9}});
    }
    SECTION("a halved source, as a pyramid level") {
        requireRender(halved(full), {.size = RenderRequest::FitInside{50, 50}});
    }
    SECTION("a quartered source, at the same fractions") {
        requireRender(halved(halved(full)), {});
    }
}

TEST_CASE("The curve-input sample under masks matches the CPU", "[gpu][local]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = sceneOf({131, 77});
    const DevelopState state =
        stateOf({adjustmentOf(someLinear(), {{"exposure", 1.0}, {"clarity", 40.0}}),
                 adjustmentOf(someRadial(), {{"relativeTint", 70.0}, {"vibrance", 50.0}})});
    const ImageBuffer expected = sample(source, state, Tap::CurveInput);
    const ImageBuffer actual = sampleOnGpu(context, source, state, Tap::CurveInput);
    REQUIRE(actual.size() == expected.size());
    REQUIRE(actual.format() == expected.format());
    // Samples are encoded for display: the bound is on the encoded values.
    const double error = worstColourError(toRgbaF32(expected), toRgbaF32(actual));
    CAPTURE(error);
    REQUIRE(error <= 2.0 * localRelativeTolerance + 1e-3);
}

TEST_CASE("The block packs the plan's masks and the bases that exist", "[gpu][local]") {
    const ImageBuffer source = sceneOf({131, 77});
    DevelopSettings settings;
    settings.presence.dehaze = 20.0F;
    const DevelopState state = stateOf(
        {adjustmentOf(someLinear(), {{"exposure", 1.0}, {"dehaze", -60.0}, {"texture", 10.0}}),
         adjustmentOf(someRadial(), {{"saturation", 25.0}}, 0.5F, true)},
        settings);
    const ProcessingPlan plan = planFor(source, state);
    const GpuPointwiseBlock block =
        packPointwise(plan.pointwise, source.size(), PointwiseProbe::Developed);
    const LocalPlan& local = plan.pointwise.local;

    REQUIRE(block.localHeader[0] == 2);
    REQUIRE(block.localHeader[1] == local.touched);
    REQUIRE(block.localHeader[2] == (static_cast<std::uint32_t>(GpuLocalFlag::FineBase) |
                                     static_cast<std::uint32_t>(GpuLocalFlag::HazeFloor) |
                                     static_cast<std::uint32_t>(GpuLocalFlag::HazeMean)));
    REQUIRE(block.localGlobal[10] == 20.0F);
    REQUIRE(block.local[0].header[0] == static_cast<std::uint32_t>(LocalMaskKind::Linear));
    REQUIRE(block.local[0].shapeA[0] == local.masks[0].alpha);
    REQUIRE(block.local[0].k[2] == 1.0F);
    REQUIRE(block.local[1].header[0] == static_cast<std::uint32_t>(LocalMaskKind::Radial));
    REQUIRE(block.local[1].header[1] == 1);
    REQUIRE(block.local[1].shapeA[2] == local.masks[1].inner);
    REQUIRE(block.local[1].shapeB[3] == local.masks[1].matrix[3]);
    REQUIRE(block.local[1].k[11] == 12.5F);
    // Unused masks are zero.
    REQUIRE(block.local[2].header[0] == 0);
    REQUIRE(block.local[15].k[0] == 0.0F);

    // No masks: the header is zero except for the Presence flags, and the globals are zero.
    const GpuPointwiseBlock plain = packPointwise(planFor(source, withoutMasks(state)).pointwise,
                                                  source.size(), PointwiseProbe::Developed);
    REQUIRE(plain.localHeader[0] == 0);
    REQUIRE(plain.localHeader[1] == 0);
    REQUIRE(plain.localGlobal == std::array<float, 16>{});
}
