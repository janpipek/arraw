#include "CheckpointState.h"
#include "ProcessingPlan.h"
#include "support/TestImages.h"

#include <Develop.h>
#include <ImagePyramid.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
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
    REQUIRE_FALSE(prefixMatches(original, changed, Stage::Resize));
    REQUIRE(prefixMatches(original, original, Stage::Resize));

    /// Each field is varied independently: full-depth comparison must agree
    /// with the defaulted equality, including fields inactive in this plan.
    const auto check = [&](const ProcessingPlan& other) {
        REQUIRE_FALSE(original == other);
        REQUIRE_FALSE(prefixMatches(original, other, Stage::Pointwise));
        REQUIRE(prefixMatches(original, other, Stage::Geometry) == (original == other));
        REQUIRE(prefixMatches(original, other, Stage::Resize) == (original == other));
        REQUIRE(prefixMatches(original, other, Stage::Effects) == (original == other));
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

    // The effects are the last group: everything before them still matches.
    changed = original;
    changed.effects.vignette.active = true;
    REQUIRE(prefixMatches(original, changed, Stage::Resize));
    REQUIRE_FALSE(prefixMatches(original, changed, Stage::Effects));
    REQUIRE(prefixMatches(original, changed, Stage::Effects) == (original == changed));

    REQUIRE_FALSE(prefixMatches(original, original, static_cast<Stage>(-1)));
    REQUIRE_FALSE(prefixMatches(original, original, static_cast<Stage>(stageCount)));
}

TEST_CASE("Plan prefixes tell requests apart at the resize and no earlier", "[plan][checkpoint]") {
    ImageBuffer source({60, 40}, workingFormat, workingEncoding);
    const DevelopSettings settings;
    const auto planned = [&](const RenderRequest& request) {
        return planFor(source, DevelopState{settings}, request);
    };
    const auto request = [](RenderRequest::Scale scale, ResizeFilter filter) {
        return RenderRequest{.size = scale, .filter = filter};
    };
    const auto half = planned(request({0.5}, ResizeFilter::Lanczos3));

    // The same request is the same plan.
    REQUIRE(half == planned(request({0.5}, ResizeFilter::Lanczos3)));
    REQUIRE(prefixMatches(half, planned(request({0.5}, ResizeFilter::Lanczos3)), Stage::Resize));

    // Another size or another filter changes what the resize computes, and
    // nothing before it.
    for (const auto& other :
         {planned(request({0.25}, ResizeFilter::Lanczos3)),
          planned(request({0.5}, ResizeFilter::Bilinear)),
          planned(RenderRequest{.size = RenderRequest::FitInside{20, 20}}), planned({})}) {
        REQUIRE(prefixMatches(half, other, Stage::Pointwise));
        REQUIRE(prefixMatches(half, other, Stage::Geometry));
        REQUIRE_FALSE(prefixMatches(half, other, Stage::Resize));
    }

    // Different requests that resolve to the same pixels are the same plan:
    // a box and a factor reaching one size, and an identity whatever its filter.
    REQUIRE(prefixMatches(half, planned(RenderRequest{.size = RenderRequest::FitInside{30, 30}}),
                          Stage::Resize));
    REQUIRE(
        prefixMatches(planned({}), planned(request({3.0}, ResizeFilter::Bilinear)), Stage::Resize));
    REQUIRE(planned({}).resize->isIdentity({60, 40}));
    REQUIRE_FALSE(half.resize->isIdentity({60, 40}));

    // The pointwise settings still decide first.
    DevelopSettings brighter;
    brighter.tone.exposure = 1.0F;
    const auto other =
        planFor(source, DevelopState{brighter}, request({0.5}, ResizeFilter::Lanczos3));
    REQUIRE_FALSE(prefixMatches(half, other, Stage::Pointwise));
}

TEST_CASE("NaN plan inputs refuse reuse even against themselves", "[plan][checkpoint]") {
    ProcessingPlan plan;
    plan.exposureGain = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(prefixMatches(plan, plan, Stage::Pointwise));
    REQUIRE_FALSE(prefixMatches(plan, plan, Stage::Geometry));
}

namespace {

/// @brief A source with a camera-ish alpha-free layout, large enough to crop and resize.
ImageBuffer resumeSource() {
    return test::rainbow({48, 32}, PixelFormat::RgbaF32, workingEncoding);
}

/// @brief Settings with some tone and a straighten, so every stage has work to do.
DevelopState resumeState(float exposure = 0.5F, double straighten = 4.0) {
    DevelopSettings settings;
    settings.tone.exposure = exposure;
    settings.tone.contrast = 20.0F;
    settings.geometry.straighten = straighten;
    return DevelopState{settings};
}

RenderRequest requestOf(std::uint32_t edge) {
    return RenderRequest{.size = RenderRequest::FitInside{edge, edge}};
}

/// @brief Requires two buffers to hold the same samples, bit for bit.
void requireIdentical(const ImageBuffer& expected, const ImageBuffer& actual) {
    REQUIRE(actual.size() == expected.size());
    REQUIRE(actual.format() == expected.format());
    REQUIRE(std::ranges::equal(actual.bytes(), expected.bytes()));
}

} // namespace

TEST_CASE("Developing until each boundary gives host checkpoints that read back as the stages",
          "[checkpoint][develop]") {
    const ImageBuffer source = resumeSource();
    const DevelopState state = resumeState();
    const RenderRequest request = requestOf(20);

    const auto pointwise = developUntil(source, state, Stage::Pointwise, request);
    REQUIRE(pointwise.boundary() == Stage::Pointwise);
    REQUIRE_FALSE(pointwise.isResident());
    REQUIRE(pointwise.size() == source.size());

    const auto geometry = developUntil(source, state, Stage::Geometry, request);
    REQUIRE(geometry.boundary() == Stage::Geometry);
    REQUIRE(geometry.size() != source.size());

    const auto resized = developUntil(source, state, Stage::Resize, request);
    REQUIRE(resized.boundary() == Stage::Resize);
    REQUIRE(resized.size().width <= 20);
    requireIdentical(develop(source, state, request), resized.readBack());

    const auto effects = developUntil(source, state, Stage::Effects, request);
    REQUIRE(effects.boundary() == Stage::Effects);
    requireIdentical(develop(source, state, request), effects.readBack());

    REQUIRE_THROWS_AS(developUntil(source, state, static_cast<Stage>(stageCount)),
                      std::invalid_argument);
}

TEST_CASE("Resuming from a pointwise checkpoint equals a full develop", "[checkpoint][develop]") {
    const ImageBuffer source = resumeSource();
    const auto checkpoint = developUntil(source, resumeState(), Stage::Pointwise);

    // A different geometry and a different request, the same pointwise result.
    const DevelopState straighter = resumeState(0.5F, -9.0);
    for (const RenderRequest& request : {RenderRequest{}, requestOf(17)}) {
        const auto resumed = resumeFrom(checkpoint, source, straighter, Stage::Resize, request);
        REQUIRE(resumed.boundary() == Stage::Resize);
        requireIdentical(develop(source, straighter, request), resumed.readBack());
    }

    const auto toGeometry = resumeFrom(checkpoint, source, straighter, Stage::Geometry);
    requireIdentical(developUntil(source, straighter, Stage::Geometry).readBack(),
                     toGeometry.readBack());
}

TEST_CASE("Resuming from a geometry checkpoint equals a full develop", "[checkpoint][develop]") {
    const ImageBuffer source = resumeSource();
    const DevelopState state = resumeState();
    const auto checkpoint = developUntil(source, state, Stage::Geometry);

    for (const RenderRequest& request :
         {requestOf(31), requestOf(9),
          RenderRequest{.size = RenderRequest::Scale{0.5}, .filter = ResizeFilter::Bilinear}}) {
        const auto resumed = resumeFrom(checkpoint, source, state, Stage::Resize, request);
        requireIdentical(develop(source, state, request), resumed.readBack());
    }
}

TEST_CASE("Resuming at the checkpoint's own boundary returns an equivalent checkpoint",
          "[checkpoint][develop]") {
    const ImageBuffer source = resumeSource();
    const DevelopState state = resumeState();
    const auto checkpoint = developUntil(source, state, Stage::Geometry);
    const auto again = resumeFrom(checkpoint, source, state, Stage::Geometry);
    REQUIRE(again.boundary() == Stage::Geometry);
    requireIdentical(checkpoint.readBack(), again.readBack());
}

TEST_CASE("A checkpoint refuses a render its plan prefix does not match", "[checkpoint][develop]") {
    const ImageBuffer source = resumeSource();
    const auto pointwise = developUntil(source, resumeState(), Stage::Pointwise);
    const auto geometry = developUntil(source, resumeState(), Stage::Geometry);

    // A changed tone setting is a different pointwise result, for both.
    const DevelopState brighter = resumeState(1.5F);
    REQUIRE_THROWS_AS(resumeFrom(pointwise, source, brighter, Stage::Resize),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(resumeFrom(geometry, source, brighter, Stage::Resize), std::invalid_argument);

    // A changed geometry is a different geometry result, but the same pointwise one.
    const DevelopState straighter = resumeState(0.5F, 8.0);
    REQUIRE_THROWS_AS(resumeFrom(geometry, source, straighter, Stage::Resize),
                      std::invalid_argument);
    REQUIRE_NOTHROW(resumeFrom(pointwise, source, straighter, Stage::Resize));

    // A resize checkpoint is for its own size and filter only.
    const auto resized = developUntil(source, resumeState(), Stage::Resize, requestOf(20));
    REQUIRE_NOTHROW(resumeFrom(resized, source, resumeState(), Stage::Resize, requestOf(20)));
    REQUIRE_THROWS_AS(resumeFrom(resized, source, resumeState(), Stage::Resize, requestOf(21)),
                      std::invalid_argument);
}

TEST_CASE("A checkpoint refuses another source or another pyramid level", "[checkpoint][develop]") {
    const ImageBuffer source = resumeSource();
    const DevelopState state = resumeState();
    const ImageBuffer reduced = halved(source);
    const ImageBuffer other = test::rainbow({40, 30}, PixelFormat::RgbaF32, workingEncoding);

    for (const Stage boundary : {Stage::Pointwise, Stage::Geometry}) {
        const auto checkpoint = developUntil(source, state, boundary);
        REQUIRE_THROWS_AS(resumeFrom(checkpoint, reduced, state, Stage::Resize),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(resumeFrom(checkpoint, other, state, Stage::Resize),
                          std::invalid_argument);
    }
    // And the same level is fine.
    REQUIRE_NOTHROW(
        resumeFrom(developUntil(reduced, state, Stage::Pointwise), reduced, state, Stage::Resize));
}

TEST_CASE("A render cannot stop before its checkpoint", "[checkpoint][develop]") {
    const ImageBuffer source = resumeSource();
    const DevelopState state = resumeState();
    const auto geometry = developUntil(source, state, Stage::Geometry);
    REQUIRE_THROWS_AS(resumeFrom(geometry, source, state, Stage::Pointwise), std::invalid_argument);
    REQUIRE_THROWS_AS(resumeFrom(geometry, source, state, static_cast<Stage>(stageCount)),
                      std::invalid_argument);
}

TEST_CASE("Asking whether a checkpoint can be resumed gives the answer the resume would",
          "[checkpoint][develop]") {
    const ImageBuffer source = resumeSource();
    const ImageBuffer reduced = halved(source);
    const DevelopState state = resumeState();
    const DevelopState brighter = resumeState(1.5F);
    const DevelopState straighter = resumeState(0.5F, 8.0);
    const auto pointwise = developUntil(source, state, Stage::Pointwise);
    const auto geometry = developUntil(source, state, Stage::Geometry);
    const auto resized = developUntil(source, state, Stage::Resize, requestOf(20));

    struct Case {
        const RenderCheckpoint* from;
        const ImageBuffer* source;
        const DevelopState* state;
        RenderRequest request;
        bool valid;
    };
    const std::array cases{
        Case{&pointwise, &source, &state, {}, true},
        Case{&pointwise, &source, &straighter, requestOf(17), true},
        Case{&pointwise, &source, &brighter, {}, false},
        Case{&pointwise, &reduced, &state, {}, false},
        Case{&geometry, &source, &state, requestOf(9), true},
        Case{&geometry, &source, &straighter, {}, false},
        Case{&geometry, &source, &brighter, {}, false},
        Case{&geometry, &reduced, &state, {}, false},
        Case{&resized, &source, &state, requestOf(20), true},
        Case{&resized, &source, &state, requestOf(21), false},
    };
    for (const Case& item : cases) {
        CAPTURE(item.from->boundary(), item.valid);
        REQUIRE(canResumeFrom(*item.from, *item.source, *item.state, Stage::Resize, item.request) ==
                item.valid);
        if (item.valid) {
            REQUIRE_NOTHROW(
                resumeFrom(*item.from, *item.source, *item.state, Stage::Resize, item.request));
        } else {
            REQUIRE_THROWS_AS(
                resumeFrom(*item.from, *item.source, *item.state, Stage::Resize, item.request),
                std::invalid_argument);
        }
    }
}

TEST_CASE("Asking whether a checkpoint can be resumed still refuses a bad request",
          "[checkpoint][develop]") {
    const ImageBuffer source = resumeSource();
    const DevelopState state = resumeState();
    const auto geometry = developUntil(source, state, Stage::Geometry);

    // A bad request is the caller's mistake, not a stale checkpoint.
    const RenderRequest backwards{.region = RenderRequest::Region{.left = 0.6, .right = 0.2}};
    REQUIRE_THROWS_AS(canResumeFrom(geometry, source, state, Stage::Resize, backwards),
                      std::invalid_argument);
    // As for the resume, a stop before the resize does not read the request.
    REQUIRE(canResumeFrom(geometry, source, state, Stage::Geometry, backwards));

    // So is a stop that is no boundary, or comes before the checkpoint's.
    REQUIRE_THROWS_AS(canResumeFrom(geometry, source, state, Stage::Pointwise),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(canResumeFrom(geometry, source, state, static_cast<Stage>(stageCount)),
                      std::invalid_argument);

    // And a state that cannot be developed.
    DevelopState broken = state;
    broken.settings.tone.exposure = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(canResumeFrom(geometry, source, broken, Stage::Resize),
                      std::invalid_argument);
}
