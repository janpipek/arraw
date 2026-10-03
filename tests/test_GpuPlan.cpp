#include "GeometryPlan.h"
#include "GpuPlan.h"
#include "ProcessingPlan.h"
#include "ToneCurve.h"

#include <ColorEncoding.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

using namespace arraw;

/// The GPU's uniform blocks are packed from the plan on the host, so their
/// contents can be checked without a device. The layout itself is held by the
/// static assertions in GpuPlan.h; these hold the values.

TEST_CASE("The pointwise block pads each matrix row to a vec4", "[gpu][plan]") {
    ProcessingPlan plan;
    plan.toWorking = Matrix3{{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.0F}};

    const auto block = packPointwise(plan);

    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            REQUIRE(block.toWorking[row * 4 + column] == plan.toWorking.at(row, column));
        }
        REQUIRE(block.toWorking[row * 4 + 3] == 0.0F);
    }
}

TEST_CASE("The pointwise block carries the tone chain's values unchanged", "[gpu][plan]") {
    const auto plan = tonePlanFor({.exposure = 1.0F,
                                   .contrast = 40.0F,
                                   .shadows = 20.0F,
                                   .highlights = -30.0F,
                                   .blacks = 10.0F,
                                   .whites = -10.0F});
    REQUIRE(plan.shapesTone);

    const auto block = packPointwise(plan);

    REQUIRE(block.exposureGain == plan.exposureGain);
    REQUIRE(block.contrastSlope == plan.contrastSlope);
    REQUIRE(block.contrastScale == plan.contrastScale);
    REQUIRE(block.shadowShift == plan.shadowShift);
    REQUIRE(block.highlightShift == plan.highlightShift);
    REQUIRE(block.blackShift == plan.blackShift);
    REQUIRE(block.whiteShift == plan.whiteShift);
    REQUIRE(block.shapesTone == 1U);
    REQUIRE(block.probe == static_cast<std::uint32_t>(PointwiseProbe::Developed));

    REQUIRE(packPointwise(ProcessingPlan{}).shapesTone == 0U);
}

TEST_CASE("The pointwise block carries the colour block unchanged", "[gpu][plan]") {
    DevelopSettings settings;
    settings.color = {.saturation = 40.0F, .vibrance = -25.0F};
    settings.hsl.red = {10.0F, 20.0F, 30.0F};
    settings.hsl.magenta = {-40.0F, -50.0F, -60.0F};
    settings.blackAndWhite = {true, 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
    const ProcessingPlan plan = planFor(ColorEncoding{workingEncoding}, DevelopState{settings});

    const auto block = packPointwise(plan);

    REQUIRE(block.saturation == plan.colorAdjustments.saturation);
    REQUIRE(block.vibrance == plan.colorAdjustments.vibrance);
    REQUIRE(block.adjustsSaturation == 1U);
    REQUIRE(block.adjustsVibrance == 1U);
    REQUIRE(block.adjustsHsl == 1U);
    REQUIRE(block.convertsToGrayscale == 1U);
    REQUIRE(block.hueShift == plan.colorAdjustments.hueShift);
    REQUIRE(block.bandSaturation == plan.colorAdjustments.bandSaturation);
    REQUIRE(block.bandLuminance == plan.colorAdjustments.bandLuminance);
    REQUIRE(block.grayMix == plan.colorAdjustments.grayMix);
    REQUIRE(block.hueShift[7] == -0.4F);
    REQUIRE(block.grayMix[7] == 8.0F);

    /// Everything off, as an unset plan is.
    const auto idle = packPointwise(ProcessingPlan{});
    REQUIRE(idle.adjustsSaturation == 0U);
    REQUIRE(idle.adjustsVibrance == 0U);
    REQUIRE(idle.adjustsHsl == 0U);
    REQUIRE(idle.convertsToGrayscale == 0U);
}

TEST_CASE("The pointwise block lays its band sets out as std140 arrays of vec4", "[gpu][plan]") {
    /// Two vec4 per set: the plan's eight contiguous floats are the same bytes.
    REQUIRE(sizeof(GpuPointwiseBlock::hueShift) == 2 * 4 * sizeof(float));
    REQUIRE(offsetof(GpuPointwiseBlock, hueShift) % 16 == 0);
    REQUIRE(offsetof(GpuPointwiseBlock, grayMix) % 16 == 0);
    REQUIRE(sizeof(GpuPointwiseBlock) % 16 == 0);
}

TEST_CASE("No shoulder reaches the shader as a flag, never as an infinity", "[gpu][plan]") {
    /// Some drivers flush infinite uniforms, or compile comparisons with them
    /// oddly, so the plan's unreachable knee is translated rather than passed.
    const auto without = packPointwise(tonePlanFor({.filmicHighlights = 0.0F}));
    REQUIRE(without.rollsHighlights == 0U);
    REQUIRE(std::isfinite(without.shoulderKnee));

    const auto plan = tonePlanFor({.filmicHighlights = 50.0F});
    REQUIRE(std::isfinite(plan.shoulderKnee));
    const auto with = packPointwise(plan);
    REQUIRE(with.rollsHighlights == 1U);
    REQUIRE(with.shoulderKnee == plan.shoulderKnee);
}

TEST_CASE("A probe is carried as the stage the shader stops after", "[gpu][plan]") {
    REQUIRE(packPointwise({}, PointwiseProbe::AfterTone).probe ==
            static_cast<std::uint32_t>(PointwiseProbe::AfterTone));
}

TEST_CASE("An identity geometry packs to an identity map", "[gpu][plan]") {
    const auto plan = geometryPlanFor({6, 4}, ImageOrientation::Normal, {});
    REQUIRE(plan.isIdentity());

    const auto block = packGeometry(plan);

    REQUIRE(block.originWhole == std::array<std::int32_t, 2>{0, 0});
    REQUIRE(block.originFraction == std::array{0.0F, 0.0F});
    REQUIRE(block.columnStepHigh == std::array{1.0F, 0.0F});
    REQUIRE(block.rowStepHigh == std::array{0.0F, 1.0F});
    REQUIRE(block.columnStepLow == std::array{0.0F, 0.0F});
    REQUIRE(block.rowStepLow == std::array{0.0F, 0.0F});
    REQUIRE(block.sourceSize == std::array<std::uint32_t, 2>{6, 4});
    REQUIRE(block.outputSize == std::array<std::uint32_t, 2>{6, 4});
}

TEST_CASE("A quarter-turn packs to integral steps from a pixel edge", "[gpu][plan]") {
    /// What keeps a quarter-turn a bit-exact copy on the GPU: every output
    /// centre lands on a source centre with no float rounding on the way.
    const auto plan = geometryPlanFor({6, 4}, ImageOrientation::Rotate90, {});
    REQUIRE_FALSE(plan.isIdentity());

    const auto block = packGeometry(plan);

    for (const float value : {block.columnStepHigh[0], block.columnStepHigh[1],
                              block.rowStepHigh[0], block.rowStepHigh[1]}) {
        REQUIRE(value == std::round(value));
    }
    for (const float value : {block.columnStepLow[0], block.columnStepLow[1], block.rowStepLow[0],
                              block.rowStepLow[1]}) {
        REQUIRE(value == 0.0F);
    }
    for (const float value : block.originFraction) {
        REQUIRE((value == 0.0F || value == 0.5F));
    }
    REQUIRE(block.outputSize == std::array<std::uint32_t, 2>{4, 6});
}

TEST_CASE("Step highs carry nine significant bits, so their products are exact", "[gpu][plan]") {
    const auto plan = geometryPlanFor({6000, 40}, ImageOrientation::Normal, {.straighten = 0.3});
    const auto block = packGeometry(plan);

    for (const float high : {block.columnStepHigh[0], block.columnStepHigh[1], block.rowStepHigh[0],
                             block.rowStepHigh[1]}) {
        int exponent = 0;
        const double mantissa = std::frexp(high, &exponent);
        // 9 bits: scaling the mantissa by 2^9 leaves an integer.
        REQUIRE(std::ldexp(mantissa, 9) == std::round(std::ldexp(mantissa, 9)));
    }
    // The largest column the shader multiplies by, at the widest permitted output.
    const float x = static_cast<float>(maxGeometryOutputExtent - 1) + 0.5F;
    const float high = block.columnStepHigh[0];
    REQUIRE(static_cast<double>(x * high) == static_cast<double>(x) * static_cast<double>(high));
    REQUIRE(block.originFraction[0] >= 0.0F);
    REQUIRE(block.originFraction[0] < 1.0F);
}

TEST_CASE("An output too wide for exact products is refused", "[gpu][plan]") {
    const auto plan =
        geometryPlanFor({maxGeometryOutputExtent + 1, 4}, ImageOrientation::Normal, {});
    REQUIRE_THROWS_AS(packGeometry(plan), std::invalid_argument);
}

TEST_CASE("The packed map lands where the CPU samples", "[gpu][plan]") {
    const auto plan = geometryPlanFor({400, 300}, ImageOrientation::Rotate270,
                                      {.flipHorizontal = true, .straighten = 7.5});
    const auto block = packGeometry(plan);

    for (const auto [x, y] :
         {std::array{0U, 0U}, std::array{plan.outputSize.width - 1, 0U}, std::array{17U, 123U},
          std::array{plan.outputSize.width - 1, plan.outputSize.height - 1}}) {
        const SourcePoint expected =
            plan.toSource({plan.left + (x + 0.5) * plan.width / plan.outputSize.width,
                           plan.top + (y + 0.5) * plan.height / plan.outputSize.height});
        const double column = x + 0.5;
        const double row = y + 0.5;
        const auto position = [&](std::size_t axis) {
            return static_cast<double>(block.originWhole[axis]) +
                   static_cast<double>(block.originFraction[axis]) +
                   column * (static_cast<double>(block.columnStepHigh[axis]) +
                             static_cast<double>(block.columnStepLow[axis])) +
                   row * (static_cast<double>(block.rowStepHigh[axis]) +
                          static_cast<double>(block.rowStepLow[axis]));
        };
        REQUIRE(std::abs(position(0) - expected.x) < 1e-5);
        REQUIRE(std::abs(position(1) - expected.y) < 1e-5);
    }
}

TEST_CASE("The pointwise block flags and carries the active tone curves", "[gpu][plan][curve]") {
    ProcessingPlan idle;
    const auto off = packPointwise(idle);
    REQUIRE(off.curvesLuma == 0U);
    REQUIRE(off.curvesRed == 0U);
    REQUIRE(off.curvesGreen == 0U);
    REQUIRE(off.curvesBlue == 0U);

    DevelopSettings settings;
    settings.toneCurve.luma.points = {{0.0F, 0.0F}, {0.5F, 0.7F}, {1.0F, 1.0F}};
    settings.toneCurve.blue.points = {{0.0F, 0.1F}, {1.0F, 0.8F}};
    const ProcessingPlan plan = planFor(workingEncoding, DevelopState{settings});

    const auto block = packPointwise(plan);
    REQUIRE(block.curvesLuma == 1U);
    REQUIRE(block.curvesRed == 0U);
    REQUIRE(block.curvesGreen == 0U);
    REQUIRE(block.curvesBlue == 1U);
}

TEST_CASE("The tone curves pack as one row, a curve to a channel", "[gpu][plan][curve]") {
    DevelopSettings settings;
    settings.toneCurve.luma.points = {{0.0F, 0.0F}, {0.5F, 0.7F}, {1.0F, 1.0F}};
    settings.toneCurve.green.points = {{0.0F, 0.1F}, {1.0F, 0.8F}};
    const ProcessingPlan plan = planFor(workingEncoding, DevelopState{settings});

    const ImageBuffer packed = packToneCurves(plan.toneCurves);
    REQUIRE(packed.size() == ImageSize{static_cast<std::uint32_t>(toneCurveSamples), 1});
    REQUIRE(packed.format() == PixelFormat::RgbaF32);

    const std::span<const float> texels = packed.samples<float>();
    for (std::size_t index = 0; index < toneCurveSamples; ++index) {
        REQUIRE(texels[index * 4] == plan.toneCurves.luma.table[index]);
        REQUIRE(texels[index * 4 + 1] == 0.0F); // red: identity, so off and zero
        REQUIRE(texels[index * 4 + 2] == plan.toneCurves.green.table[index]);
        REQUIRE(texels[index * 4 + 3] == 0.0F);
    }
    // The ends are the curve's own, which the shader's extension starts from.
    REQUIRE(texels[2] == Catch::Approx(0.1F));
    REQUIRE(texels[(toneCurveSamples - 1) * 4 + 2] == Catch::Approx(0.8F));

    // Default settings pack to an all-zero texture of the same size.
    const ImageBuffer idle = packToneCurves(ToneCurvePlan{});
    REQUIRE(idle.size() == packed.size());
    for (const float value : idle.samples<float>()) {
        REQUIRE(value == 0.0F);
    }
}
