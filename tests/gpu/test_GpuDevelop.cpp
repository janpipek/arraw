#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuTesting.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImagePyramid.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef ARRAW_TEST_DATA_DIR
#error "ARRAW_TEST_DATA_DIR must name the fixture directory; see tests/CMakeLists.txt"
#endif

using namespace arraw;
using namespace arraw::test;

namespace {

/// @brief Largest colour error of a pointwise chain followed by a resample.
///
/// The pointwise error is relative to the pixel's scale and the resample is a
/// weighted mean of pixels, so the two add at worst.
constexpr double endToEndTolerance = pointwiseRelativeTolerance + resampleTolerance;

/// @brief Largest absolute alpha difference: alpha only passes through the resample.
constexpr double endToEndAlphaTolerance = resampleTolerance;

/// @brief Largest absolute alpha difference between two RGBA float images.
double worstAlphaError(const ImageBuffer& expected, const ImageBuffer& actual) {
    const std::span<const float> want = expected.samples<float>();
    const std::span<const float> got = actual.samples<float>();
    double worst = 0.0;
    for (std::size_t index = 3; index < want.size(); index += 4) {
        worst = std::max(worst, std::abs(static_cast<double>(want[index]) - got[index]));
    }
    return worst;
}

/// @brief Requires the device's development of a source to match the CPU's.
void requireMatchesCpu(const ImageBuffer& source, const DevelopSettings& settings) {
    const ImageBuffer expected = develop(source, DevelopState{settings});
    const ImageBuffer actual =
        developOnGpu(gpuContext(), source, DevelopState{settings}).readBack();
    REQUIRE(actual.format() == PixelFormat::RgbaF32);
    REQUIRE(actual.size() == expected.size());
    const double colour = worstColourError(expected, actual);
    const double alpha = worstAlphaError(expected, actual);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "end to end: colour %.3g, alpha %.3g\n", colour, alpha);
    }
    CAPTURE(colour, alpha, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    REQUIRE(colour <= endToEndTolerance);
    REQUIRE(alpha <= endToEndAlphaTolerance);
}

/// @brief Builds settings from a tone, a colour and a geometry, each a little of everything.
DevelopSettings combined(WhiteBalanceMode mode, GeometrySettings geometry) {
    DevelopSettings settings;
    settings.tone = {0.4F, 30.0F, 30.0F, -30.0F, 10.0F, 20.0F, 70.0F};
    settings.color = {mode, 4200.0F, 8.0F};
    settings.geometry = geometry;
    return settings;
}

/// @brief Names the geometry cases, each with more than one operation.
std::array<std::pair<const char*, GeometrySettings>, 6> geometryCases() {
    return {{
        {"straighten and auto crop", {.straighten = 7.5}},
        {"quarter-turn, flips, straighten",
         {.rotation = QuarterTurn::Clockwise90, .flipHorizontal = true, .straighten = -12.0}},
        {"half-turn, vertical flip, explicit crop",
         {.rotation = QuarterTurn::Clockwise180,
          .flipVertical = true,
          .straighten = 3.0,
          .crop = {.rectangle =
                       UprightCropRect{.left = 0.1, .top = 0.2, .right = 0.8, .bottom = 0.9}}}},
        {"three quarters and a ratio crop",
         {.rotation = QuarterTurn::Clockwise270,
          .flipHorizontal = true,
          .flipVertical = true,
          .straighten = 20.0,
          .crop = {.aspect = CropRatio{3.0 / 2.0}}}},
        {"flips only", {.flipHorizontal = true, .flipVertical = true}},
        {"nothing", {}},
    }};
}

/// @brief Loads a fixture with its own encoding and orientation.
ImageBuffer fixtureImage(const char* name) {
    return loadImage(std::filesystem::path(ARRAW_TEST_DATA_DIR) / name);
}

} // namespace

TEST_CASE("Developing on the GPU matches the CPU on RAW fixtures", "[gpu][develop]") {
    const std::array fixtures{"linear-32x24-neutral.dng", "linear-32x24-warmwb.dng",
                              "linear-32x24-nowb.dng",    "linear-32x24-rotated.dng",
                              "linear-32x24-skewed.dng",  "bayer-32x24.dng"};
    for (const char* name : fixtures) {
        const ImageBuffer source = fixtureImage(name);
        for (const auto& [label, geometry] : geometryCases()) {
            for (const WhiteBalanceMode mode :
                 {WhiteBalanceMode::AsShot, WhiteBalanceMode::Custom}) {
                DYNAMIC_SECTION(name << ", " << label << ", "
                                     << (mode == WhiteBalanceMode::Custom ? "custom" : "as shot")) {
                    requireMatchesCpu(source, combined(mode, geometry));
                }
            }
        }
    }
}

TEST_CASE("Developing on the GPU matches the CPU on PNG fixtures", "[gpu][develop]") {
    // Colour settings do not apply to a non-RAW photograph; tone and geometry do.
    for (const char* name : {"testcard-61x41-srgb8.png", "testcard-61x41-srgb16.png",
                             "testcard-61x41-alpha8.png", "testcard-61x41-adobergb8.png"}) {
        const ImageBuffer source = fixtureImage(name);
        for (const auto& [label, geometry] : geometryCases()) {
            DYNAMIC_SECTION(name << ", " << label) {
                requireMatchesCpu(source, combined(WhiteBalanceMode::AsShot, geometry));
            }
        }
    }
}

TEST_CASE("Developing on the GPU with default settings matches the CPU", "[gpu][develop]") {
    for (const char* name : {"testcard-61x41-srgb8.png", "linear-32x24-rotated.dng"}) {
        DYNAMIC_SECTION(name) {
            requireMatchesCpu(fixtureImage(name), DevelopSettings{});
        }
    }
}

TEST_CASE("Developing from an uploaded source equals developing from the host", "[gpu][develop]") {
    GpuContext& context = gpuContext();
    for (const char* name : {"testcard-61x41-alpha8.png", "linear-32x24-rotated.dng"}) {
        DYNAMIC_SECTION(name) {
            const ImageBuffer source = fixtureImage(name);
            const DeviceImage uploaded = uploadSource(context, source);
            REQUIRE(uploaded.size() == source.size());
            const DevelopState state{combined(WhiteBalanceMode::AsShot, {.straighten = 7.5})};
            const RenderRequest request{.size = RenderRequest::FitInside{20, 20}};

            const ImageBuffer direct =
                developOnGpu(context, source, state, Stage::Resize, request).readBack();
            const ImageBuffer viaUpload =
                developOnGpu(context, source, uploaded, state, Stage::Resize, request).readBack();

            REQUIRE(viaUpload.size() == direct.size());
            // The same passes on the same texels: no tolerance is needed.
            REQUIRE(compareFloat(direct, viaUpload, pointwiseAbsoluteFloor).bitExact);
        }
    }
}

TEST_CASE("One uploaded source serves renders of different states", "[gpu][develop]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = fixtureImage("testcard-61x41-srgb8.png");
    const DeviceImage uploaded = uploadSource(context, source);

    for (const float exposure : {-1.0F, 0.0F, 0.7F}) {
        DYNAMIC_SECTION("exposure " << exposure) {
            DevelopSettings settings;
            settings.tone.exposure = exposure;
            const DevelopState state{settings};
            const ImageBuffer expected = develop(source, state);
            const ImageBuffer actual = developOnGpu(context, source, uploaded, state).readBack();
            REQUIRE(actual.size() == expected.size());
            REQUIRE(worstColourError(expected, actual) <= endToEndTolerance);
        }
    }
    // The upload is left as it was: it still reads back as the source.
    REQUIRE(uploaded.size() == source.size());
}

TEST_CASE("An uploaded source of another device or another size is refused", "[gpu][develop]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = fixtureImage("testcard-61x41-srgb8.png");
    const ImageBuffer smaller = fixtureImage("linear-32x24-rotated.dng");

    SECTION("another device") {
        GpuContext other(gpuTestBackend());
        const DeviceImage foreign = uploadSource(other, source);
        REQUIRE_THROWS_AS(developOnGpu(context, source, foreign, DevelopState{}),
                          std::invalid_argument);
    }
    SECTION("another size") {
        const DeviceImage wrongSize = uploadSource(context, smaller);
        REQUIRE_THROWS_AS(developOnGpu(context, source, wrongSize, DevelopState{}),
                          std::invalid_argument);
    }
    SECTION("an empty image") {
        REQUIRE_THROWS_AS(developOnGpu(context, source, DeviceImage{}, DevelopState{}),
                          std::invalid_argument);
    }
}

TEST_CASE("Developing a halved source on the GPU matches the CPU", "[gpu][develop][pyramid]") {
    // What the preview does with a pyramid level: the reduced copy is uploaded
    // and developed like any source.
    GpuContext& context = gpuContext();
    const ImageBuffer reduced = halved(fixtureImage("testcard-61x41-srgb16.png"));
    const DevelopState state{combined(WhiteBalanceMode::AsShot, {.straighten = 5.0})};
    const RenderRequest request{.size = RenderRequest::FitInside{20, 20}};

    const ImageBuffer expected = develop(reduced, state, request);
    const DeviceImage uploaded = uploadSource(context, reduced);
    const ImageBuffer actual =
        developOnGpu(context, reduced, uploaded, state, Stage::Resize, request).readBack();

    REQUIRE(actual.size() == expected.size());
    const double colour = worstColourError(expected, actual);
    const double alpha = worstAlphaError(expected, actual);
    CAPTURE(colour, alpha, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    REQUIRE(colour <= endToEndTolerance);
    REQUIRE(alpha <= endToEndAlphaTolerance);
}

namespace {

/// @brief Requires a resumed render to equal a fresh one on the device, pass for pass.
void requireResumeEqualsFresh(GpuContext& context, const RenderCheckpoint& checkpoint,
                              const ImageBuffer& source, const DevelopState& state,
                              const RenderRequest& request) {
    const std::size_t before = context.renderCount();
    const RenderCheckpoint resumed =
        developOnGpu(context, checkpoint, source, state, Stage::Resize, request);
    const std::size_t resumedPasses = context.renderCount() - before;
    const RenderCheckpoint fresh = developOnGpu(context, source, state, Stage::Resize, request);
    const std::size_t freshPasses = context.renderCount() - before - resumedPasses;

    REQUIRE(resumed.isResident());
    REQUIRE(resumed.boundary() == Stage::Resize);
    REQUIRE(resumedPasses < freshPasses);
    const ImageBuffer expected = fresh.readBack();
    const ImageBuffer actual = resumed.readBack();
    REQUIRE(actual.size() == expected.size());
    REQUIRE(compareFloat(expected, actual, pointwiseAbsoluteFloor).bitExact);
    REQUIRE(worstColourError(develop(source, state, request), actual) <= endToEndTolerance);
}

} // namespace

TEST_CASE("Resuming on the GPU from a pointwise or a geometry checkpoint equals a fresh render",
          "[gpu][develop][checkpoint]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = fixtureImage("testcard-61x41-alpha8.png");
    const DevelopState state{combined(WhiteBalanceMode::AsShot, {.straighten = 7.5})};
    const RenderRequest request{.size = RenderRequest::FitInside{20, 20}};

    const RenderCheckpoint pointwise = developOnGpu(context, source, state, Stage::Pointwise);
    const RenderCheckpoint geometry = developOnGpu(context, source, state, Stage::Geometry);

    SECTION("a viewport change after a geometry checkpoint") {
        requireResumeEqualsFresh(context, geometry, source, state,
                                 RenderRequest{.size = RenderRequest::FitInside{33, 33}});
    }
    SECTION("a geometry change after a pointwise checkpoint") {
        const DevelopState straighter{combined(WhiteBalanceMode::AsShot, {.straighten = -3.0})};
        requireResumeEqualsFresh(context, pointwise, source, straighter, request);
    }
    SECTION("stopping at the geometry after a pointwise checkpoint") {
        const RenderCheckpoint resumed =
            developOnGpu(context, pointwise, source, state, Stage::Geometry);
        REQUIRE(resumed.boundary() == Stage::Geometry);
        REQUIRE(
            compareFloat(geometry.readBack(), resumed.readBack(), pointwiseAbsoluteFloor).bitExact);
    }
    SECTION("the checkpoint's own boundary costs no pass") {
        const std::size_t before = context.renderCount();
        const RenderCheckpoint again =
            developOnGpu(context, geometry, source, state, Stage::Geometry);
        REQUIRE(context.renderCount() == before);
        REQUIRE(again.boundary() == Stage::Geometry);
    }
}

TEST_CASE("Resuming on the GPU refuses what the checkpoint cannot serve",
          "[gpu][develop][checkpoint]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = fixtureImage("testcard-61x41-srgb8.png");
    const DevelopState state{combined(WhiteBalanceMode::AsShot, {.straighten = 7.5})};
    const RenderCheckpoint pointwise = developOnGpu(context, source, state, Stage::Pointwise);
    const RenderCheckpoint geometry = developOnGpu(context, source, state, Stage::Geometry);

    SECTION("a host checkpoint") {
        const RenderCheckpoint host = developUntil(source, state, Stage::Pointwise);
        REQUIRE_THROWS_AS(developOnGpu(context, host, source, state), std::invalid_argument);
    }
    SECTION("a checkpoint from another device") {
        GpuContext other(gpuTestBackend());
        const RenderCheckpoint foreign = developOnGpu(other, source, state, Stage::Pointwise);
        REQUIRE_THROWS_AS(developOnGpu(context, foreign, source, state), std::invalid_argument);
    }
    SECTION("a changed tone setting") {
        DevelopSettings brighter = combined(WhiteBalanceMode::AsShot, {.straighten = 7.5});
        brighter.tone.exposure += 1.0F;
        REQUIRE_THROWS_AS(developOnGpu(context, pointwise, source, DevelopState{brighter}),
                          std::invalid_argument);
    }
    SECTION("a changed geometry after a geometry checkpoint") {
        const DevelopState straighter{combined(WhiteBalanceMode::AsShot, {.straighten = 1.0})};
        REQUIRE_THROWS_AS(developOnGpu(context, geometry, source, straighter),
                          std::invalid_argument);
    }
    SECTION("another pyramid level") {
        const ImageBuffer reduced = halved(source);
        REQUIRE_THROWS_AS(developOnGpu(context, pointwise, reduced, state), std::invalid_argument);
    }
    SECTION("stopping before the checkpoint") {
        REQUIRE_THROWS_AS(developOnGpu(context, geometry, source, state, Stage::Pointwise),
                          std::invalid_argument);
    }
}

TEST_CASE("The CPU refuses a checkpoint that lives on a device", "[gpu][develop][checkpoint]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = fixtureImage("testcard-61x41-srgb8.png");
    const DevelopState state{};
    const RenderCheckpoint resident = developOnGpu(context, source, state, Stage::Pointwise);
    REQUIRE(resident.isResident());
    REQUIRE_THROWS_AS(resumeFrom(resident, source, state, Stage::Resize), std::invalid_argument);
}
