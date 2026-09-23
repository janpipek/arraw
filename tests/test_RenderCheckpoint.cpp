#include "CheckpointState.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace arraw;

TEST_CASE("Host checkpoints share immutable pixels and read back independent copies",
          "[checkpoint]") {
    ImageBuffer source({1, 1}, workingFormat, workingEncoding);
    const std::array values{-0.25F, 2.0F, 0.5F, 0.3F};
    std::ranges::copy(values, source.samples<float>().begin());
    const auto* storage = source.bytes().data();
    const auto checkpoint = makeCheckpoint(Stage::Pointwise, {}, std::move(source));
    const auto copy = checkpoint;

    REQUIRE(checkpoint.boundary() == Stage::Pointwise);
    REQUIRE(checkpoint.size() == ImageSize{1, 1});
    REQUIRE(checkpoint.encoding() == ColorEncoding{workingEncoding});
    REQUIRE_FALSE(checkpoint.isResident());
    REQUIRE(&copy.encoding() == &checkpoint.encoding());

    auto first = checkpoint.readBack();
    const auto second = copy.readBack();
    REQUIRE(first.bytes().data() != storage);
    REQUIRE(first.bytes().data() != second.bytes().data());
    REQUIRE(std::ranges::equal(first.samples<float>(), values));
    REQUIRE(first.format() == workingFormat);
    first.samples<float>()[0] = 99.0F;
    REQUIRE(std::ranges::equal(second.samples<float>(), values));
    REQUIRE(std::ranges::equal(checkpoint.readBack().samples<float>(), values));
}

TEST_CASE("Checkpoints refuse invalid boundaries and missing pixels", "[checkpoint]") {
    REQUIRE_THROWS_AS(RenderCheckpoint{nullptr}, std::invalid_argument);
    REQUIRE_THROWS_AS(makeCheckpoint(Stage::Pointwise, {}, DeviceImage{}), std::invalid_argument);
    REQUIRE_THROWS_AS(makeCheckpoint(static_cast<Stage>(stageCount), {},
                                     ImageBuffer({1, 1}, workingFormat, workingEncoding)),
                      std::invalid_argument);

    ImageBuffer source({1, 1}, workingFormat, workingEncoding);
    const auto held = makeCheckpoint(Stage::Geometry, {}, std::move(source));
    REQUIRE(held.boundary() == Stage::Geometry);
    REQUIRE_THROWS_AS(makeCheckpoint(Stage::Pointwise, {}, std::move(source)),
                      std::invalid_argument);

    const auto empty = std::make_shared<const CheckpointState>(
        CheckpointState{Stage::Pointwise, {}, DeviceImage{}});
    REQUIRE_THROWS_AS(RenderCheckpoint{empty}, std::invalid_argument);
}

TEST_CASE("An empty device image has no device or readable pixels", "[checkpoint][device]") {
    const DeviceImage image;
    REQUIRE_FALSE(image.valid());
    REQUIRE(image.device() == DeviceId::None);
    REQUIRE(image.size().empty());
    REQUIRE(image.format() == workingFormat);
    REQUIRE_THROWS_AS(image.encoding(), std::logic_error);
    REQUIRE_THROWS_AS(image.readBack(), std::logic_error);
}

TEST_CASE("Plan prefixes ignore later geometry but include every pointwise input",
          "[plan][checkpoint]") {
    const ProcessingPlan original;
    auto changed = original;
    changed.geometry = geometryPlanFor({8, 6}, ImageOrientation::Normal, {});
    REQUIRE(prefixMatches(original, changed, Stage::Pointwise));
    REQUIRE_FALSE(prefixMatches(original, changed, Stage::Geometry));
    REQUIRE(prefixMatches(original, changed, Stage::Geometry) == (original == changed));

    /// Each field is varied independently: full-depth comparison must agree
    /// with the defaulted equality, including fields inactive in this plan.
    const auto check = [&](const ProcessingPlan& other) {
        REQUIRE_FALSE(original == other);
        REQUIRE_FALSE(prefixMatches(original, other, Stage::Pointwise));
        REQUIRE(prefixMatches(original, other, Stage::Geometry) == (original == other));
    };
    changed = original;
    changed.toWorking = Matrix3{};
    check(changed);
    changed = original;
    changed.shapesTone = true;
    check(changed);
    for (auto field : {&ProcessingPlan::exposureGain, &ProcessingPlan::contrastSlope,
                       &ProcessingPlan::contrastScale, &ProcessingPlan::shadowShift,
                       &ProcessingPlan::highlightShift, &ProcessingPlan::blackShift,
                       &ProcessingPlan::whiteShift, &ProcessingPlan::shoulderKnee}) {
        changed = original;
        changed.*field = 0.25F;
        check(changed);
    }
    REQUIRE(prefixMatches(original, original, Stage::Geometry) == (original == original));
    REQUIRE_FALSE(prefixMatches(original, original, static_cast<Stage>(-1)));
    REQUIRE_FALSE(prefixMatches(original, original, static_cast<Stage>(stageCount)));
}

TEST_CASE("NaN plan inputs refuse reuse even against themselves", "[plan][checkpoint]") {
    ProcessingPlan plan;
    plan.exposureGain = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(prefixMatches(plan, plan, Stage::Pointwise));
    REQUIRE_FALSE(prefixMatches(plan, plan, Stage::Geometry));
}
