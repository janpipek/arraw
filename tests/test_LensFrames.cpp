// Brush strokes and masks under lens corrections: the frames of ADR 009 as ADR 050, section 7
// realises them.

#include "BrushCoverage.h"
#include "BrushCoverageCache.h"
#include "BrushRaster.h"
#include "Lens.h"
#include "LocalPlan.h"
#include "ProcessingPlan.h"
#include "support/BrushGenerators.h"
#include "support/LadderTesting.h"
#include "support/LensTables.h"

#include <BrushStrokes.h>
#include <Develop.h>
#include <DevelopState.h>
#include <DevelopedFrame.h>
#include <ImageBuffer.h>
#include <LensCorrection.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <utility>
#include <variant>
#include <vector>

using namespace arraw;
using Catch::Approx;

namespace {

constexpr ImageSize raster{300, 200};
constexpr double aspect = 1.5;

/// @brief The sensor point of the feature: near a corner, where a barrel moves things the most.
constexpr SensorPoint feature{0.86F, 0.14F};

/// @brief Makes the one-point stroke that paints a spot.
std::shared_ptr<const StrokeList> spotAt(SensorPoint at, float radius = 0.02F) {
    return std::make_shared<const StrokeList>(
        std::vector<Stroke>{Stroke{radius, 1.0F, 1.0F, false, {at}}});
}

struct Centroid {
    double x = 0.0;
    double y = 0.0;
};

/// @brief Gives the centroid of values above a floor, in pixel coordinates.
template <typename At> Centroid centroidOf(ImageSize size, double floor, const At& at) {
    double sum = 0.0;
    Centroid centroid;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double value = at(x, y) - floor;
            if (value > 0.0) {
                sum += value;
                centroid.x += value * (x + 0.5);
                centroid.y += value * (y + 0.5);
            }
        }
    }
    REQUIRE(sum > 0.0);
    centroid.x /= sum;
    centroid.y /= sum;
    return centroid;
}

/// @brief Makes the scene: dark grey, with a soft bright spot at a sensor point.
ImageBuffer sceneWithSpot(SensorPoint at) {
    ImageBuffer image(raster, workingFormat, workingEncoding);
    auto samples = image.samples<float>();
    const double cx = at.u * raster.width;
    const double cy = at.v * raster.height;
    for (std::uint32_t y = 0; y < raster.height; ++y) {
        for (std::uint32_t x = 0; x < raster.width; ++x) {
            const double dx = x + 0.5 - cx;
            const double dy = y + 0.5 - cy;
            const float spot =
                static_cast<float>(std::exp(-(dx * dx + dy * dy) / (2.0 * 3.0 * 3.0)));
            float* pixel = &samples[(static_cast<std::size_t>(y) * raster.width + x) * 4];
            pixel[0] = 0.05F + 0.6F * spot;
            pixel[1] = 0.05F + 0.6F * spot;
            pixel[2] = 0.05F + 0.6F * spot;
            pixel[3] = 1.0F;
        }
    }
    return image;
}

} // namespace

TEST_CASE("A stroke painted on a feature of the sensor frame lands on it in the corrected one",
          "[lens][brush][frames]") {
    const LensCorrection barrel = test::barrelCorrection(-0.10F, aspect);
    const auto frame = brushFrameOf(barrel);
    REQUIRE(frame != nullptr);

    // Where the lens stage puts the feature.
    const ImageBuffer corrected = detail::applyLens(sceneWithSpot(feature), LensPlan{barrel},
                                                    detail::LensResampler::CatmullRom);
    const auto pixels = corrected.samples<float>();
    const Centroid subject = centroidOf(raster, 0.05, [&](std::uint32_t x, std::uint32_t y) {
        return static_cast<double>(pixels[(static_cast<std::size_t>(y) * raster.width + x) * 4]);
    });

    // Where the stroke's coverage is drawn, through the lens and without it.
    const CoveragePlane through = rasteriseBrush(*spotAt(feature), raster, frame.get());
    const Centroid painted = centroidOf(raster, 0.0, [&](std::uint32_t x, std::uint32_t y) {
        return static_cast<double>(through.values[static_cast<std::size_t>(y) * raster.width + x]);
    });
    CHECK(painted.x == Approx(subject.x).margin(0.35));
    CHECK(painted.y == Approx(subject.y).margin(0.35));

    // Without the map it would be somewhere else: the lens moved the feature several pixels.
    const CoveragePlane unmapped = rasteriseBrush(*spotAt(feature), raster);
    const Centroid stayed = centroidOf(raster, 0.0, [&](std::uint32_t x, std::uint32_t y) {
        return static_cast<double>(unmapped.values[static_cast<std::size_t>(y) * raster.width + x]);
    });
    CHECK(std::hypot(stayed.x - subject.x, stayed.y - subject.y) > 3.0);
    CHECK(stayed.x == Approx(feature.u * raster.width).margin(0.01));
}

TEST_CASE("A stroke keeps its radius and its sensor-frame points under the lens",
          "[lens][brush][frames]") {
    const LensCorrection barrel = test::barrelCorrection(-0.10F, aspect);
    const auto frame = brushFrameOf(barrel);
    const Stroke stroke{0.03F, 1.0F, 1.0F, false, {{0.3F, 0.4F}, {0.5F, 0.4F}, {0.7F, 0.45F}}};
    const PlacedStroke plain = placedStroke(stroke, raster);
    const PlacedStroke mapped = placedStroke(stroke, raster, frame.get());
    CHECK(mapped.radius == plain.radius);
    CHECK(mapped.inner == plain.inner);
    CHECK(mapped.flow == plain.flow);
    // The dabs are those of the sensor path, each moved: as many, the same prefix property.
    REQUIRE(mapped.dabs.size() == plain.dabs.size());
    CHECK(mapped.regular == plain.regular);
    // Each is where the lens inverse puts the sensor-frame dab.
    for (std::size_t i = 0; i < plain.dabs.size(); ++i) {
        const auto expected =
            correctedFrom(barrel, SensorPoint{static_cast<float>(plain.dabs[i].x / raster.width),
                                              static_cast<float>(plain.dabs[i].y / raster.height)});
        REQUIRE(expected.has_value());
        CHECK(mapped.dabs[i].x == Approx(expected->u * raster.width).margin(1e-3));
        CHECK(mapped.dabs[i].y == Approx(expected->v * raster.height).margin(1e-3));
    }
}

TEST_CASE("Only distortion moves a brush: the frame of a correction without it is none",
          "[lens][brush][frames]") {
    CHECK(brushFrameOf(LensCorrection{}) == nullptr);
    CHECK(brushFrameOf(test::vignettingCorrection(0.5F, aspect)) == nullptr);
    const auto frame = brushFrameOf(test::barrelCorrection(-0.1F, aspect));
    REQUIRE(frame != nullptr);
    // What it keeps is what places a point, and nothing of the other corrections.
    LensCorrection both = test::barrelCorrection(-0.1F, aspect);
    both.vignetting = std::vector<float>(lensTableNodes, 1.4F);
    both.vignettingFrom = LensProvenance{.lens = {"Test", "Lens"}};
    CHECK(sameBrushFrame(brushFrameOf(both), frame));
    CHECK(brushFrameOf(both)->vignetting.empty());
    CHECK_FALSE(brushFrameOf(both)->vignettingFrom.has_value());
}

TEST_CASE("The coverage cache keeps one entry per frame, and a vignetting toggle finds the old one",
          "[lens][brush][frames][cache]") {
    BrushCoverageCache cache(std::size_t{256} << 20, 64);
    const auto strokes = spotAt({0.5F, 0.5F}, 0.1F);
    const auto distortion = brushFrameOf(test::barrelCorrection(-0.1F, aspect));
    const auto other = brushFrameOf(test::barrelCorrection(-0.2F, aspect));

    CHECK(cache.coverage(strokes, raster).lookup == CoverageLookup::Miss);
    CHECK(cache.peek(strokes, raster).lookup == CoverageLookup::Hit);
    // The same strokes through a lens are another coverage, and the first is not disturbed.
    CHECK(cache.peek(strokes, raster, distortion).lookup == CoverageLookup::Miss);
    CHECK(cache.find(strokes, raster, distortion) == nullptr);
    CHECK(cache.coverage(strokes, raster, true, distortion).lookup == CoverageLookup::Miss);
    CHECK(cache.coverage(strokes, raster, true, other).lookup == CoverageLookup::Miss);
    CHECK(cache.entryCount() == 3);
    CHECK(cache.coverage(strokes, raster).lookup == CoverageLookup::Hit);
    // An equal frame behind another pointer is the same one.
    const auto again = brushFrameOf(test::barrelCorrection(-0.1F, aspect));
    CHECK(again != distortion);
    CHECK(cache.coverage(strokes, raster, true, again).lookup == CoverageLookup::Hit);
    CHECK(cache.entryCount() == 3);
}

TEST_CASE("Coverage grown or extended through a lens is the reference drawn through it",
          "[lens][brush][frames][cache]") {
    const auto frame = brushFrameOf(test::barrelCorrection(-0.12F, aspect));
    std::mt19937_64 g(20261018);
    BrushCoverageCache cache(std::size_t{256} << 20, 64);
    const auto before = test::paintedMask(20261019, 2, test::everydayStyle, 0.667);
    const auto held = cache.coverage(before, raster, true, frame);
    CHECK(held.lookup == CoverageLookup::Miss);
    CHECK(held.tiles->gathered() == rasteriseBrush(*before, raster, frame.get()));

    Stroke path = test::wanderingStroke(g, test::detailStyle, 0.667);
    path.points.resize(80);
    for (std::size_t points = 1; points <= 80; points += 13) {
        Stroke partial = path;
        partial.points.resize(points);
        const auto list = before->appended(partial);
        const auto result = cache.coverage(list, raster, true, frame);
        CHECK(result.lookup == (points == 1 ? CoverageLookup::Extended : CoverageLookup::Grown));
        REQUIRE(result.tiles->gathered() == rasteriseBrush(*list, raster, frame.get()));
    }
}

TEST_CASE("A toggle changes the brush of a plan, a vignetting toggle and a develop edit do not",
          "[lens][brush][frames][plan]") {
    LocalAdjustment adjustment;
    adjustment.shape = BrushMask{spotAt({0.5F, 0.5F}, 0.1F)};
    adjustment.deltas.exposure = 1.0F;
    const DevelopState state = withLocalAdjustmentAdded(DevelopState{}, adjustment);
    const ImageBuffer source = sceneWithSpot({0.5F, 0.5F});

    const auto brushOf = [&](const DevelopState& developed, const LensCorrection& lens) {
        const ProcessingPlan plan = planFor(source, developed, lens, {});
        REQUIRE(plan.pointwise.local.masks.size() == 1);
        return plan.pointwise.local.masks.front().brush;
    };
    const BrushCoverageRef off = brushOf(state, LensCorrection{});
    const BrushCoverageRef distorted = brushOf(state, test::barrelCorrection(-0.1F, aspect));
    CHECK(off.frame == nullptr);
    CHECK(distorted.frame != nullptr);
    CHECK_FALSE(off == distorted);

    LensCorrection both = test::barrelCorrection(-0.1F, aspect);
    both.vignetting = std::vector<float>(lensTableNodes, 1.3F);
    CHECK(brushOf(state, both) == distorted);
    CHECK(brushOf(state, test::vignettingCorrection(0.3F, aspect)) == off);

    DevelopState edited = state;
    edited.settings.tone.exposure = 0.5F;
    CHECK(brushOf(edited, test::barrelCorrection(-0.1F, aspect)) == distorted);
}

TEST_CASE("A brush painted on a feature lifts the feature in a developed picture with the lens",
          "[lens][brush][frames][develop]") {
    const LensCorrection barrel = test::barrelCorrection(-0.10F, aspect);
    const ImageBuffer source = sceneWithSpot(feature);
    LocalAdjustment adjustment;
    adjustment.shape = BrushMask{spotAt(feature, 0.03F)};
    adjustment.deltas.exposure = 1.5F;
    const DevelopState brushed = withLocalAdjustmentAdded(DevelopState{}, adjustment);

    const ImageBuffer with = develop(source, brushed, barrel, {});
    const ImageBuffer without = develop(source, DevelopState{}, barrel, {});
    const auto a = with.samples<float>();
    const auto b = without.samples<float>();
    REQUIRE(a.size() == b.size());
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t index = (static_cast<std::size_t>(y) * raster.width + x) * 4;
        return static_cast<double>(a[index] - b[index]);
    };
    const Centroid lifted = centroidOf(raster, 0.002, at);
    const Centroid subject = centroidOf(raster, 0.05, [&](std::uint32_t x, std::uint32_t y) {
        return static_cast<double>(b[(static_cast<std::size_t>(y) * raster.width + x) * 4]);
    });
    CHECK(lifted.x == Approx(subject.x).margin(0.6));
    CHECK(lifted.y == Approx(subject.y).margin(0.6));

    // And it is another picture than the same brush without the lens, where it is somewhere else.
    const ImageBuffer unmapped = develop(source, brushed, LensCorrection{}, {});
    CHECK_FALSE(test::sameBits(unmapped, with));
}

TEST_CASE("A linear mask's contour is straight in the corrected frame, lens or no lens",
          "[lens][mask][frames]") {
    const LensCorrection barrel = test::barrelCorrection(-0.15F, aspect);
    LocalAdjustment adjustment;
    adjustment.shape = LinearMask{{0.3F, 0.5F}, {0.7F, 0.5F}};
    adjustment.deltas.exposure = 1.0F;
    DevelopState state = withLocalAdjustmentAdded(DevelopState{}, adjustment);
    const DevelopedFrameMap map({raster, ImageOrientation::Normal}, {});

    const ImageSize grid{120, 80};
    const MaskCoverage plain =
        maskCoverage(state.localAdjustments.front(), map, {}, grid, LensCorrection{});
    const MaskCoverage lensed = maskCoverage(state.localAdjustments.front(), map, {}, grid, barrel);
    // Not moved by the lens at all: masks live in the corrected frame.
    CHECK(lensed.weights == plain.weights);

    // The 50 % contour of a vertical ramp is the same column on every row.
    std::vector<int> crossings;
    for (std::uint32_t row = 0; row < grid.height; ++row) {
        int crossing = -1;
        for (std::uint32_t column = 1; column < grid.width; ++column) {
            const auto before = lensed.weights[row * grid.width + column - 1];
            const auto after = lensed.weights[row * grid.width + column];
            if ((before < 128) != (after < 128)) {
                crossing = static_cast<int>(column);
                break;
            }
        }
        REQUIRE(crossing >= 0);
        crossings.push_back(crossing);
    }
    for (const int crossing : crossings) {
        CHECK(crossing == crossings.front());
    }

    // The same through a render: the mask's lift is the same function of the corrected pixel
    // with and without the lens, so a flat picture comes out as flat as the lens leaves it.
    ImageBuffer flat(raster, workingFormat, workingEncoding);
    auto samples = flat.samples<float>();
    std::ranges::fill(samples, 0.25F);
    const ImageBuffer withLens = develop(flat, state, barrel, {});
    const ImageBuffer withoutLens = develop(flat, state, LensCorrection{}, {});
    CHECK(test::sameBits(withLens, withoutLens));
}

TEST_CASE("A brush painted through a lens has the sensor frame's points back by the inverse",
          "[lens][brush][frames]") {
    const LensCorrection barrel = test::barrelCorrection(-0.10F, aspect);
    for (const SensorPoint point : {SensorPoint{0.1F, 0.1F}, SensorPoint{0.9F, 0.2F},
                                    SensorPoint{0.5F, 0.5F}, SensorPoint{0.95F, 0.95F}}) {
        const auto corrected = correctedFrom(barrel, point);
        REQUIRE(corrected.has_value());
        const SensorPoint back = sensorFrom(barrel, *corrected);
        CHECK(back.u == Approx(point.u).margin(1e-5));
        CHECK(back.v == Approx(point.v).margin(1e-5));
    }
}
