#include "GeometryPlan.h"
#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuPlan.h"
#include "GpuTesting.h"

#include <DevelopSettings.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

using namespace arraw;
using namespace arraw::test;

namespace {

/// Largest relative difference tolerated on a large, opaque, straightened image.
///
/// Measured worst on 1000x700 at 0.3, 7.5 and -30 degrees: 1.4e-4 (absolute
/// 4e-5). The float position error grows with the source length: a ramp image
/// shows up to 2.4e-4 pixels at 1000 columns, four ulps of the coordinate.
constexpr double largeImageTolerance = 3.0e-4;

/// Magnitude below which the transparent-neighbourhood comparison counts against a floor.
constexpr double transparentFloor = 5.0e-2;

/// Largest relative difference tolerated on a large image with transparent pixels.
///
/// Measured worst 5.0e-3 above ::transparentFloor. Where most neighbours are
/// transparent the colour is a ratio of two small weighted sums, so the same
/// float weight error is magnified by the inverse of the total alpha.
constexpr double transparentTolerance = 1.0e-2;

/// Largest absolute difference tolerated on a large image with transparent pixels.
///
/// Measured worst 1.6e-3, in colour beside almost transparent neighbours.
constexpr double transparentAbsoluteTolerance = 3.0e-3;

ImageBuffer labelled(ImageSize size, bool opaque = false) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    auto samples = image.samples<float>();
    for (std::size_t index = 0; index < size.pixelCount(); ++index) {
        const auto tag = static_cast<float>(index + 1);
        samples[index * 4] = tag / static_cast<float>(size.pixelCount() + 1);
        samples[index * 4 + 1] = 0.25F + 0.5F * static_cast<float>((index * 7) % 13) / 13.0F;
        samples[index * 4 + 2] = 0.5F + 0.4F * static_cast<float>((index * 5) % 11) / 11.0F;
        float alpha = 1.0F;
        if (opaque) {
            alpha = 1.0F;
        } else if (index == 0 || index % 17 == 5) {
            alpha = 0.0F; // Transparent, but coloured.
        } else if (index % 3 == 1) {
            alpha = 0.25F + 0.5F * static_cast<float>(index % 7) / 7.0F;
        }
        samples[index * 4 + 3] = alpha;
    }
    return image;
}

/// @brief Views a block as the bytes a render takes.
template <typename Block> std::span<const std::byte> bytesOf(const Block& block) {
    return std::as_bytes(std::span<const Block, 1>(&block, 1));
}

/// @brief Resamples on the device and on the host, and compares the two.
///
/// The uploaded buffer stays Normal, as develop() leaves its pixels; the
/// camera orientation reaches the plan alone.
FloatDifference compareGeometry(const ImageBuffer& source, ImageOrientation orientation,
                                const GeometrySettings& settings,
                                double floor = geometryAbsoluteFloor) {
    GpuContext& context = gpuContext();
    const GeometryPlan plan = geometryPlanFor(source.size(), orientation, settings);
    const GpuGeometryBlock block = packGeometry(plan);
    const ImageBuffer actual = context
                                   .render(GpuPass::Geometry, bytesOf(block),
                                           context.upload(source), plan.outputSize, workingEncoding)
                                   .readBack();
    const ImageBuffer expected = applyGeometry(source.clone(), plan);
    REQUIRE(actual.size() == plan.outputSize);
    return compareFloat(expected, actual, floor);
}

/// @brief Reports a measured error when asked, for choosing the tolerances.
void measure(const char* what, const FloatDifference& difference) {
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(
            stderr, "%s: max abs %.3g, max rel %.3g (sample %d,%d,%d: %g vs %g)%s\n", what,
            difference.maxAbsDiff, difference.maxRelDiff, difference.worstX, difference.worstY,
            difference.worstChannel, static_cast<double>(difference.worstExpected),
            static_cast<double>(difference.worstActual), difference.bitExact ? " bit-exact" : "");
    }
}

constexpr std::array allOrientations{
    ImageOrientation::Normal,     ImageOrientation::MirrorHorizontal,
    ImageOrientation::Rotate180,  ImageOrientation::MirrorVertical,
    ImageOrientation::Transpose,  ImageOrientation::Rotate90,
    ImageOrientation::Transverse, ImageOrientation::Rotate270};

constexpr std::array allTurns{QuarterTurn::None, QuarterTurn::Clockwise90,
                              QuarterTurn::Clockwise180, QuarterTurn::Clockwise270};

} // namespace

TEST_CASE("The geometry pass copies samples bit for bit through orientations and quarter-turns",
          "[gpu][geometry]") {
    for (const ImageOrientation orientation : allOrientations) {
        for (const QuarterTurn turn : allTurns) {
            for (const ImageSize size : {ImageSize{5, 3}, ImageSize{4, 4}}) {
                CAPTURE(static_cast<int>(orientation), static_cast<int>(turn), size.width);
                const auto difference =
                    compareGeometry(labelled(size), orientation, {.rotation = turn});
                measure("orientation and turn", difference);
                CHECK(difference.bitExact);
            }
        }
    }
}

TEST_CASE("The geometry pass flips bit for bit", "[gpu][geometry]") {
    for (const bool horizontal : {false, true}) {
        for (const bool vertical : {false, true}) {
            for (const QuarterTurn turn : allTurns) {
                CAPTURE(horizontal, vertical, static_cast<int>(turn));
                const auto difference = compareGeometry(
                    labelled({7, 4}), ImageOrientation::Rotate90,
                    {.rotation = turn, .flipHorizontal = horizontal, .flipVertical = vertical});
                measure("flips", difference);
                CHECK(difference.bitExact);
            }
        }
    }
}

TEST_CASE("The geometry pass copies a pixel-aligned crop bit for bit", "[gpu][geometry]") {
    const auto difference = compareGeometry(
        labelled({8, 6}), ImageOrientation::Normal,
        {.crop = {.rectangle = UprightCropRect{
                      .left = 0.25, .top = 1.0 / 3, .right = 0.75, .bottom = 5.0 / 6}}});
    measure("aligned crop", difference);
    CHECK(difference.bitExact);
}

TEST_CASE("The geometry pass straightens like the CPU", "[gpu][geometry]") {
    for (const double angle : {-45.0, -7.5, -0.3, 0.3, 7.5, 45.0}) {
        for (const ImageOrientation orientation :
             {ImageOrientation::Normal, ImageOrientation::Rotate90}) {
            CAPTURE(angle, static_cast<int>(orientation));
            const auto difference =
                compareGeometry(labelled({23, 17}), orientation, {.straighten = angle});
            measure("straighten", difference);
            CHECK(difference.maxRelDiff <= resampleTolerance);
        }
    }
}

TEST_CASE("The geometry pass crops like the CPU", "[gpu][geometry]") {
    const ImageBuffer source = labelled({31, 20});
    SECTION("automatic crop of a straightened image") {
        const auto difference =
            compareGeometry(source, ImageOrientation::Normal, {.straighten = 12.0});
        measure("auto crop", difference);
        CHECK(difference.maxRelDiff <= resampleTolerance);
    }
    SECTION("explicit fractional crop") {
        const auto difference = compareGeometry(
            source, ImageOrientation::Normal,
            {.crop = {.rectangle = UprightCropRect{
                          .left = 0.13, .top = 0.21, .right = 0.71, .bottom = 0.93}}});
        measure("explicit crop", difference);
        CHECK(difference.maxRelDiff <= resampleTolerance);
    }
    SECTION("explicit crop of a straightened image") {
        const auto difference =
            compareGeometry(source, ImageOrientation::Normal,
                            {.straighten = -4.0,
                             .crop = {.rectangle = UprightCropRect{
                                          .left = 0.3, .top = 0.3, .right = 0.7, .bottom = 0.7}}});
        measure("explicit crop straightened", difference);
        CHECK(difference.maxRelDiff <= resampleTolerance);
    }
    SECTION("locked aspects") {
        for (const double ratio : {3.0 / 2.0, 2.0 / 3.0}) {
            CAPTURE(ratio);
            const auto difference =
                compareGeometry(source, ImageOrientation::Normal,
                                {.straighten = 5.0, .crop = {.aspect = CropRatio{ratio}}});
            measure("ratio crop", difference);
            CHECK(difference.maxRelDiff <= resampleTolerance);
        }
        const auto difference =
            compareGeometry(source, ImageOrientation::Rotate90,
                            {.straighten = 5.0, .crop = {.aspect = OriginalCropAspect{}}});
        measure("original aspect crop", difference);
        CHECK(difference.maxRelDiff <= resampleTolerance);
    }
}

TEST_CASE("The geometry pass handles one-pixel-wide and one-pixel-tall images", "[gpu][geometry]") {
    for (const ImageSize size : {ImageSize{1, 9}, ImageSize{9, 1}, ImageSize{1, 1}}) {
        CAPTURE(size.width, size.height);
        for (const ImageOrientation orientation : allOrientations) {
            const auto difference = compareGeometry(labelled(size), orientation, {});
            measure("1xN orientation", difference);
            CHECK(difference.bitExact);
        }
        const auto difference =
            compareGeometry(labelled(size), ImageOrientation::Normal, {.straighten = 20.0});
        measure("1xN straighten", difference);
        CHECK(difference.maxRelDiff <= resampleTolerance);
    }
}

TEST_CASE("The geometry pass keeps the CPU's accuracy on a large straightened image",
          "[gpu][geometry]") {
    for (const double angle : {0.3, 7.5, -30.0}) {
        CAPTURE(angle);
        const auto opaque = compareGeometry(labelled({1000, 700}, true), ImageOrientation::Normal,
                                            {.straighten = angle});
        measure("large opaque", opaque);
        CHECK(opaque.maxRelDiff <= largeImageTolerance);

        const auto transparent = compareGeometry(labelled({1000, 700}), ImageOrientation::Normal,
                                                 {.straighten = angle}, transparentFloor);
        measure("large transparent", transparent);
        CHECK(transparent.maxRelDiff <= transparentTolerance);
        CHECK(transparent.maxAbsDiff <= transparentAbsoluteTolerance);
    }
}

TEST_CASE("Developing on the GPU stops at the pass boundaries with the planned sizes",
          "[gpu][geometry]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = labelled({40, 30});
    const DevelopSettings settings{.geometry = {.straighten = 10.0}};
    const GeometryPlan plan =
        geometryPlanFor(source.size(), source.orientation(), settings.geometry);
    REQUIRE_FALSE(plan.isIdentity());

    const RenderCheckpoint pointwise = developOnGpu(context, source, settings, Stage::Pointwise);
    CHECK(pointwise.isResident());
    CHECK(pointwise.boundary() == Stage::Pointwise);
    CHECK(pointwise.size() == source.size());

    const RenderCheckpoint geometry = developOnGpu(context, source, settings, Stage::Geometry);
    CHECK(geometry.isResident());
    CHECK(geometry.boundary() == Stage::Geometry);
    CHECK(geometry.size() == plan.outputSize);
    CHECK(geometry.size() != source.size());
}
