#include "GeometryPlan.h"
#include "GpuPlan.h"
#include "ProcessingPlan.h"

#include <ColorEncoding.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

    REQUIRE(block.origin == std::array{0.0F, 0.0F});
    REQUIRE(block.columnStep == std::array{1.0F, 0.0F});
    REQUIRE(block.rowStep == std::array{0.0F, 1.0F});
    REQUIRE(block.sourceSize == std::array<std::uint32_t, 2>{6, 4});
    REQUIRE(block.outputSize == std::array<std::uint32_t, 2>{6, 4});
}

TEST_CASE("A quarter-turn packs to integral steps from a pixel edge", "[gpu][plan]") {
    /// What keeps a quarter-turn a bit-exact copy on the GPU: every output
    /// centre lands on a source centre with no float rounding on the way.
    const auto plan = geometryPlanFor({6, 4}, ImageOrientation::Rotate90, {});
    REQUIRE_FALSE(plan.isIdentity());

    const auto block = packGeometry(plan);

    for (const float value : {block.origin[0], block.origin[1], block.columnStep[0],
                              block.columnStep[1], block.rowStep[0], block.rowStep[1]}) {
        REQUIRE(value == std::round(value));
    }
    REQUIRE(block.outputSize == std::array<std::uint32_t, 2>{4, 6});
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
        const float column = static_cast<float>(x) + 0.5F;
        const float row = static_cast<float>(y) + 0.5F;
        const float sourceX =
            block.origin[0] + column * block.columnStep[0] + row * block.rowStep[0];
        const float sourceY =
            block.origin[1] + column * block.columnStep[1] + row * block.rowStep[1];
        REQUIRE(std::abs(sourceX - expected.x) < 1e-3);
        REQUIRE(std::abs(sourceY - expected.y) < 1e-3);
    }
}
