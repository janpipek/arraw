#include "Effects.h"
#include "GrainModels.h"
#include "ProcessingPlan.h"

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
#include <vector>

using namespace arraw;

/// Grain: its plan, its seed, the value-noise model and its band-limit (ADR 038).

namespace {

/// @brief The grey every grain test develops, in linear light.
constexpr float grey = 0.5F;

/// @brief Settings under which the pointwise chain leaves a working-space image as it is.
DevelopSettings grainy(float amount, float size = 50.0F, float roughness = 50.0F,
                       std::uint32_t seed = 0) {
    DevelopSettings settings;
    settings.tone = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    settings.effects.grain = {.amount = amount, .size = size, .roughness = roughness, .seed = seed};
    return settings;
}

/// @brief A working-space image of one grey everywhere.
ImageBuffer greyOf(ImageSize size) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::size_t index = 0; index < samples.size(); index += 4) {
        samples[index] = grey;
        samples[index + 1] = grey;
        samples[index + 2] = grey;
        samples[index + 3] = 1.0F;
    }
    return image;
}

/// @brief A field of numbers over a render's pixels, row by row.
struct Field {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<double> values;

    [[nodiscard]] double at(std::uint32_t x, std::uint32_t y) const {
        return values[static_cast<std::size_t>(y) * width + x];
    }
};

/// @brief What grain added to a developed grey, in the perceptual coordinate, from its red channel.
Field grainOf(const ImageBuffer& developed) {
    const ImageSize size = developed.size();
    Field field{size.width, size.height, {}};
    const auto samples = developed.samples<float>();
    const double base = toPerceptualSigned(grey);
    for (std::size_t index = 0; index < samples.size(); index += 4) {
        field.values.push_back(toPerceptualSigned(samples[index]) - base);
    }
    return field;
}

/// @brief The grain a placement draws over a render, straight from the model.
Field grainOf(const GrainPlan& plan, const GrainPlacement& placement, ImageSize size) {
    Field field{size.width, size.height, {}};
    field.values.reserve(static_cast<std::size_t>(size.width) * size.height);
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            field.values.push_back(grainAt(plan, placement, x, y));
        }
    }
    return field;
}

/// @brief The grain a plan draws over a render, straight from the model.
Field grainOf(const GrainPlan& plan, const FrameMapping& mapping, ImageSize size) {
    return grainOf(plan, grainPlacementOf(plan, mapping), size);
}

/// @brief The mapping of the top-left `size` pixels of a square frame rendered `edge` pixels wide.
FrameMapping cornerOf(double edge) {
    return {.origin = {0.0, 0.0}, .step = {1.0 / edge, 1.0 / edge}, .aspect = 1.0};
}

double meanOf(const Field& field) {
    double sum = 0.0;
    for (const double value : field.values) {
        sum += value;
    }
    return sum / static_cast<double>(field.values.size());
}

double deviationOf(const Field& field) {
    const double mean = meanOf(field);
    double sum = 0.0;
    for (const double value : field.values) {
        sum += (value - mean) * (value - mean);
    }
    return std::sqrt(sum / static_cast<double>(field.values.size()));
}

/// @brief Pearson correlation of two fields of one size.
double correlationOf(const Field& a, const Field& b) {
    REQUIRE(a.values.size() == b.values.size());
    const double meanA = meanOf(a);
    const double meanB = meanOf(b);
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (std::size_t index = 0; index < a.values.size(); ++index) {
        const double x = a.values[index] - meanA;
        const double y = b.values[index] - meanB;
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }
    return ab / std::sqrt(aa * bb);
}

/// @brief Averages each `factor` by `factor` block of a field: what a smaller render can show.
Field boxedDown(const Field& field, std::uint32_t factor) {
    Field result{field.width / factor, field.height / factor, {}};
    for (std::uint32_t y = 0; y < result.height; ++y) {
        for (std::uint32_t x = 0; x < result.width; ++x) {
            double sum = 0.0;
            for (std::uint32_t dy = 0; dy < factor; ++dy) {
                for (std::uint32_t dx = 0; dx < factor; ++dx) {
                    sum += field.at(x * factor + dx, y * factor + dy);
                }
            }
            result.values.push_back(sum / (factor * factor));
        }
    }
    return result;
}

void requireIdentical(const ImageBuffer& expected, const ImageBuffer& actual) {
    REQUIRE(actual.size() == expected.size());
    REQUIRE(std::ranges::equal(actual.bytes(), expected.bytes()));
}

/// @brief Largest difference between a field and a window of another, in the same units.
double worstDifference(const Field& part, const Field& whole, std::uint32_t left,
                       std::uint32_t top) {
    double worst = 0.0;
    for (std::uint32_t y = 0; y < part.height; ++y) {
        for (std::uint32_t x = 0; x < part.width; ++x) {
            worst = std::max(worst, std::abs(part.at(x, y) - whole.at(left + x, top + y)));
        }
    }
    return worst;
}

GrainPlan planOf(float amount, float size = 50.0F, float roughness = 50.0F,
                 std::uint32_t seed = 0) {
    return grainPlanFor(grainy(amount, size, roughness, seed).effects.grain);
}

/// @brief Positions of a region render are worked out apart from the whole frame's, in double,
/// then narrowed: the grain they find differs by float rounding of the lattice position only.
constexpr double regionTolerance = 1e-5;

} // namespace

TEST_CASE("Grain at zero amount is off whatever else it says, and the plan stays the default",
          "[effects][grain]") {
    REQUIRE(grainPlanFor({}) == GrainPlan{});
    for (const float size : {0.0F, 100.0F}) {
        for (const float roughness : {0.0F, 70.0F}) {
            for (const std::uint32_t seed : {0U, 12345U}) {
                REQUIRE(planOf(0.0F, size, roughness, seed) == GrainPlan{});
            }
        }
    }
    REQUIRE(planOf(0.5F).active);
    // Out of range clamps; not finite, or an unknown model, is refused.
    REQUIRE(planOf(300.0F) == planOf(100.0F));
    REQUIRE(planOf(50.0F, -20.0F) == planOf(50.0F, 0.0F));
    REQUIRE_THROWS_AS(planOf(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
    REQUIRE_THROWS_AS(planOf(10.0F, std::numeric_limits<float>::infinity()), std::invalid_argument);
    GrainSettings unknown{.amount = 10.0F};
    unknown.model = static_cast<GrainModel>(7);
    REQUIRE_THROWS_AS(grainPlanFor(unknown), std::invalid_argument);

    // Off on a frame: the same pixels, the same plan and no pass, as without grain.
    const ImageBuffer source = greyOf({40, 30});
    const RenderRequest request{.size = RenderRequest::FitInside{25, 25}};
    const DevelopState plain{grainy(0.0F)};
    const DevelopState shaped{grainy(0.0F, 90.0F, 10.0F, 777U)};
    requireIdentical(develop(source, plain, request), develop(source, shaped, request));
    REQUIRE(planFor(source, plain, request) == planFor(source, shaped, request));
    requireIdentical(developUntil(source, shaped, Stage::Resize, request).readBack(),
                     develop(source, shaped, request));
}

TEST_CASE("The plan resolves size, roughness and deviation from the settings", "[effects][grain]") {
    const GrainPlan finest = planOf(100.0F, 0.0F, 0.0F);
    const GrainPlan coarsest = planOf(100.0F, 100.0F, 100.0F);
    REQUIRE(finest.deviation == strongestGrainDeviation);
    REQUIRE(std::abs(finest.size - finestGrain) <= 1e-6 * finestGrain);
    REQUIRE(std::abs(coarsest.size - coarsestGrain) <= 1e-6 * coarsestGrain);
    REQUIRE(finest.roughness == 0.0F);
    REQUIRE(coarsest.roughness == 1.0F);
    // Size is spread on a logarithmic scale: the middle is the geometric mean.
    REQUIRE(std::abs(planOf(1.0F, 50.0F).size - std::sqrt(finestGrain * coarsestGrain)) <=
            1e-6 * finestGrain);
    REQUIRE(planOf(25.0F).deviation == 0.25F * strongestGrainDeviation);
}

TEST_CASE("A seed of zero renders one fixed pattern", "[effects][grain][seed]") {
    REQUIRE(planOf(40.0F).seed == unseededGrainSeed);
    REQUIRE(planOf(40.0F, 50.0F, 50.0F, 9U).seed == 9U);
    const ImageBuffer source = greyOf({64, 48});
    requireIdentical(
        develop(source, DevelopState{grainy(40.0F, 100.0F)}),
        develop(source, DevelopState{grainy(40.0F, 100.0F, 50.0F, unseededGrainSeed)}));
}

TEST_CASE("Only an edit that turns grain on without a seed gets a new one",
          "[effects][grain][seed]") {
    const GrainEntropy drawn = [] { return 0xdeadbeefU; };
    const GrainSettings off{};
    const GrainSettings on{.amount = 30.0F};
    // Left off, or turned off: nothing to choose, so the seed stays as it is.
    REQUIRE(chooseGrainSeed(off, off, drawn) == 0U);
    REQUIRE(chooseGrainSeed(off, GrainSettings{.seed = 5U}, drawn) == 5U);
    REQUIRE(chooseGrainSeed(on, off, drawn) == 0U);
    // Already on: kept, so a photograph's grain never re-rolls, not even the
    // fixed pattern of seed zero.
    REQUIRE(chooseGrainSeed(on, GrainSettings{.amount = 60.0F}, drawn) == 0U);
    REQUIRE(chooseGrainSeed(on, GrainSettings{.amount = 60.0F, .seed = 5U}, drawn) == 5U);
    // Turned on with a seed: kept.
    REQUIRE(chooseGrainSeed(off, GrainSettings{.amount = 30.0F, .seed = 5U}, drawn) == 5U);
    // Turned on without: drawn, and never zero, which means "none".
    REQUIRE(chooseGrainSeed(off, on, drawn) == 0xdeadbeefU);
    REQUIRE(chooseGrainSeed(GrainSettings{.seed = 0U}, on, [] { return 0U; }) != 0U);
    // An amount the plan clamps to zero, or not a number, is off.
    REQUIRE(chooseGrainSeed(GrainSettings{.amount = -5.0F}, on, drawn) == 0xdeadbeefU);
    // The operating system's entropy by default.
    REQUIRE(chooseGrainSeed(off, on) != 0U);
}

TEST_CASE("Grain is zero-mean, with the plan's deviation, at every roughness",
          "[effects][grain][statistics]") {
    // The top-left 512 by 512 pixels of a 2048-pixel frame, at the coarsest
    // size: four pixels a cell, so nothing is faded, and about 16,000 cells
    // of the finest lattice.
    const ImageSize size{512, 512};
    for (const float roughness : {0.0F, 50.0F, 100.0F}) {
        CAPTURE(roughness);
        const GrainPlan plan = planOf(100.0F, 100.0F, roughness);
        const Field field = grainOf(plan, cornerOf(2048.0), size);
        const double mean = meanOf(field);
        const double deviation = deviationOf(field);
        CAPTURE(mean, deviation);
        REQUIRE(std::abs(mean) <= 0.03 * plan.deviation);
        REQUIRE(std::abs(deviation / plan.deviation - 1.0) <= 0.06);
    }
    // The amount scales the grain and nothing else.
    const Field full = grainOf(planOf(100.0F, 100.0F), cornerOf(2048.0), {128, 128});
    const Field half = grainOf(planOf(50.0F, 100.0F), cornerOf(2048.0), {128, 128});
    for (std::size_t index = 0; index < full.values.size(); ++index) {
        REQUIRE(std::abs(half.values[index] - 0.5 * full.values[index]) <= 1e-7);
    }
    // On a developed grey, in the perceptual coordinate, the same: a frame
    // 1024 pixels long, so two pixels a cell, the finest drawn in full.
    const Field developed =
        grainOf(develop(greyOf({1024, 256}), DevelopState{grainy(100.0F, 100.0F, 0.0F)}));
    CAPTURE(meanOf(developed), deviationOf(developed));
    REQUIRE(std::abs(meanOf(developed)) <= 0.03 * strongestGrainDeviation);
    REQUIRE(std::abs(deviationOf(developed) / strongestGrainDeviation - 1.0) <= 0.06);
}

TEST_CASE("Grain is the same on every render, and another seed is another pattern",
          "[effects][grain][seed]") {
    // Enlarged eight times, so that the grain is coarse enough to be drawn.
    const ImageBuffer source = greyOf({96, 64});
    const RenderRequest request{.size = RenderRequest::Scale{8.0}, .upscale = Upscale::Allowed};
    const DevelopState state{grainy(60.0F, 100.0F, 40.0F, 42U)};
    const ImageBuffer first = develop(source, state, request);
    requireIdentical(first, develop(source, state, request));
    const ImageBuffer other =
        develop(source, DevelopState{grainy(60.0F, 100.0F, 40.0F, 43U)}, request);
    REQUIRE_FALSE(std::ranges::equal(first.bytes(), other.bytes()));

    const ImageSize size{256, 256};
    const Field a = grainOf(planOf(60.0F, 100.0F, 40.0F, 1U), cornerOf(1024.0), size);
    const Field b = grainOf(planOf(60.0F, 100.0F, 40.0F, 2U), cornerOf(1024.0), size);
    CAPTURE(correlationOf(a, b));
    REQUIRE(std::abs(correlationOf(a, b)) <= 0.1);
}

TEST_CASE("A point of the frame gets the same grain whatever region or zoom renders it",
          "[effects][grain][region][slow]") {
    // Coarse grain wherever the frame is a thousand pixels or more, so that
    // every comparison below has grain to compare.
    const DevelopState coarse{grainy(80.0F, 100.0F, 60.0F, 7U)};
    const double visible = 0.3 * planOf(80.0F).deviation;

    // At one to one, a region is the pixels of the whole frame it covers.
    const ImageBuffer wide = greyOf({1024, 512});
    const Field whole = grainOf(develop(wide, coarse));
    const Field region =
        grainOf(develop(wide, coarse, {.region = RenderRequest::Region{0.25, 0.5, 0.75, 1.0}}));
    REQUIRE(region.width == 512);
    REQUIRE(deviationOf(region) > visible);
    REQUIRE(worstDifference(region, whole, 256, 256) <= regionTolerance);

    // Zoomed in by four: the whole frame, and a region of it.
    const ImageBuffer source = greyOf({256, 256});
    const RenderRequest zoom{.size = RenderRequest::Scale{4.0}, .upscale = Upscale::Allowed};
    RenderRequest zoomedRegion = zoom;
    zoomedRegion.region = RenderRequest::Region{0.5, 0.25, 1.0, 0.75};
    const Field big = grainOf(develop(source, coarse, zoom));
    const Field part = grainOf(develop(source, coarse, zoomedRegion));
    REQUIRE(part.width == 512);
    REQUIRE(deviationOf(part) > visible);
    REQUIRE(worstDifference(part, big, 512, 256) <= regionTolerance);

    // Far in, where a whole frame would be 4096 pixels wide, with the finest
    // grain and so the most cells: two overlapping regions find the same
    // grain where they overlap, so the positions hold at any zoom.
    const DevelopState fine{grainy(80.0F, 0.0F, 60.0F, 7U)};
    const RenderRequest deep{.size = RenderRequest::Scale{16.0}, .upscale = Upscale::Allowed};
    RenderRequest first = deep;
    first.region = RenderRequest::Region{0.375, 0.375, 0.5, 0.5};
    RenderRequest second = deep;
    second.region = RenderRequest::Region{0.4375, 0.4375, 0.5625, 0.5625};
    const Field a = grainOf(develop(source, fine, first));
    const Field b = grainOf(develop(source, fine, second));
    REQUIRE(a.width == 512);
    Field overlap{256, 256, {}};
    for (std::uint32_t y = 0; y < 256; ++y) {
        for (std::uint32_t x = 0; x < 256; ++x) {
            overlap.values.push_back(b.at(x, y));
        }
    }
    // The finest lattice is a pixel a cell there, and so left out for its
    // substitute; the two coarser ones remain.
    REQUIRE(deviationOf(overlap) > 0.2 * planOf(80.0F).deviation);
    REQUIRE(worstDifference(overlap, a, 256, 256) <= regionTolerance);
}

TEST_CASE("Grain finer than a pixel fades into a pixel-scale substitute instead of aliasing",
          "[effects][grain][band]") {
    const GrainPlan plan = planOf(100.0F, 0.0F, 0.0F);
    // The finest grain is half a pixel of a 2048-pixel edge: a pixel of a
    // 4096-pixel one, where the lattice is left out; two of an 8192-pixel
    // one, where it is whole; and faded in between.
    const GrainPlacement gone = grainPlacementOf(plan, cornerOf(4096.0));
    const GrainPlacement between = grainPlacementOf(plan, cornerOf(6144.0));
    const GrainPlacement whole = grainPlacementOf(plan, cornerOf(8192.0));
    REQUIRE(gone.layers[0].weight == 0.0F);
    REQUIRE(between.layers[0].weight > 0.0F);
    REQUIRE(between.layers[0].weight < whole.layers[0].weight);
    // What the fade leaves out goes to the substitute, which a lattice drawn
    // whole does not need: there the placement is as it was.
    REQUIRE(gone.layers[valueNoiseSubstituteLayer].weight > 0.0F);
    REQUIRE(between.layers[valueNoiseSubstituteLayer].weight > 0.0F);
    REQUIRE(whole.layers[valueNoiseSubstituteLayer].weight == 0.0F);
    // So the grain never vanishes: what is drawn is about what a box filter of
    // the pixel keeps of the whole grain, more of it the larger the render.
    const double goneDeviation = deviationOf(grainOf(plan, cornerOf(4096.0), {256, 256}));
    const double betweenDeviation = deviationOf(grainOf(plan, cornerOf(6144.0), {256, 256}));
    const double wholeDeviation = deviationOf(grainOf(plan, cornerOf(8192.0), {256, 256}));
    CAPTURE(goneDeviation, betweenDeviation, wholeDeviation, plan.deviation);
    REQUIRE(goneDeviation > 0.6 * plan.deviation);
    REQUIRE(goneDeviation < betweenDeviation);
    REQUIRE(betweenDeviation < wholeDeviation);
    REQUIRE(std::abs(wholeDeviation / plan.deviation - 1.0) <= 0.06);
}

TEST_CASE("Grain at the default size stays visible on an ordinary export",
          "[effects][grain][band][slow]") {
    // An 1800 by 1200 render: the default size is 1.24 pixels a cell of the
    // finest lattice there, which the band-limit fades nearly out. The
    // substitute keeps what a box filter of the pixel would keep of it.
    // Developed at roughness 50, the default; from the model at roughness 0,
    // the finest lattice alone.
    const Field grain = grainOf(develop(greyOf({1800, 1200}), DevelopState{grainy(100.0F)}));
    const double developed = deviationOf(grain) / strongestGrainDeviation;
    const GrainPlan even = planOf(100.0F, 50.0F, 0.0F);
    const double model = deviationOf(grainOf(even, cornerOf(1800.0), {512, 512})) / even.deviation;
    CAPTURE(developed, model);
    REQUIRE(std::abs(meanOf(grain)) <= 0.03 * strongestGrainDeviation);
    for (const double share : {developed, model}) {
        REQUIRE(share >= 0.75);
        REQUIRE(share <= 0.95);
    }
    // The finest size, at roughness zero, on a 1600-pixel render: cells of
    // 0.39 pixels, all of the grain below the band-limit, and still there.
    const GrainPlan finest = planOf(100.0F, 0.0F, 0.0F);
    const double share =
        deviationOf(grainOf(finest, cornerOf(1600.0), {512, 512})) / finest.deviation;
    CAPTURE(share);
    REQUIRE(share >= 0.4);
    REQUIRE(share <= 0.7);
}

TEST_CASE("A smaller render is as grainy as a larger one downscaled to its size",
          "[effects][grain][band][preview][slow]") {
    // The top-left 1024 pixels of an 8192-pixel frame, where every lattice is
    // two pixels a cell or more, boxed down to what renders 4 and 8 times
    // smaller would show; those renders draw it with the substitute.
    for (const float size : {0.0F, 40.0F}) {
        for (const float roughness : {0.0F, 50.0F, 100.0F}) {
            const GrainPlan plan = planOf(100.0F, size, roughness, 21U);
            const Field full = grainOf(plan, cornerOf(8192.0), {1024, 1024});
            REQUIRE(
                grainPlacementOf(plan, cornerOf(8192.0)).layers[valueNoiseSubstituteLayer].weight ==
                0.0F);
            for (const std::uint32_t factor : {4U, 8U}) {
                CAPTURE(size, roughness, factor);
                const double edge = 8192.0 / factor;
                const std::uint32_t pixels = 1024U / factor;
                const double reduced = deviationOf(grainOf(plan, cornerOf(edge), {pixels, pixels}));
                const double boxed = deviationOf(boxedDown(full, factor));
                CAPTURE(reduced, boxed, reduced / boxed);
                REQUIRE(std::abs(reduced / boxed - 1.0) <= 0.15);
            }
        }
    }
}

TEST_CASE("A preview softens grain but never moves what it can show",
          "[effects][grain][preview][slow]") {
    // A pyramid level and the full frame, the level rendered at a quarter of
    // the size; the full-resolution render boxed down to that size is what the
    // preview should resemble. Coarse, clustered grain on a 2048-pixel edge:
    // its cells are 4, 7.5 and 17 pixels at full size, 1, 1.9 and 4.3 in the
    // preview, which leaves out the finest and keeps the other two.
    const ImageBuffer source = greyOf({2048, 512});
    const ImageBuffer level = halved(halved(source));
    const RenderRequest preview{.size = RenderRequest::FitInside{512, 512}};
    const DevelopState state{grainy(100.0F, 100.0F, 100.0F, 3U)};
    const Field full = grainOf(develop(source, state));
    const Field reduced = grainOf(develop(level, state, preview));
    REQUIRE(reduced.width == 512);
    const Field boxed = boxedDown(full, 4);
    const Field elsewhere =
        boxedDown(grainOf(develop(source, DevelopState{grainy(100.0F, 100.0F, 100.0F, 4U)})), 4);

    // Alike, though the preview draws the finest grain as its substitute,
    // another pattern (about 0.33 here); against another seed's grain,
    // nothing in common.
    const double same = correlationOf(reduced, boxed);
    const double other = correlationOf(reduced, elsewhere);
    CAPTURE(same, other, deviationOf(reduced), deviationOf(boxed), deviationOf(full));
    REQUIRE(same >= 0.25);
    REQUIRE(std::abs(other) <= 0.1);
    // Without the substitute, what the preview draws of the grain is what the
    // full render shows of it, boxed down.
    const GrainPlan plan = planOf(100.0F, 100.0F, 100.0F, 3U);
    const FrameMapping levelMapping{
        .origin = {0.0, 0.0}, .step = {1.0 / 512.0, 1.0 / 128.0}, .aspect = 4.0};
    GrainPlacement drawn = grainPlacementOf(plan, levelMapping);
    drawn.layers[valueNoiseSubstituteLayer].weight = 0.0F;
    const Field lattices = grainOf(plan, drawn, {512, 128});
    CAPTURE(correlationOf(lattices, boxed));
    REQUIRE(correlationOf(lattices, boxed) >= 0.5);
    // Softer: less deviation than at full size, never more; and about as much
    // as the full render boxed down (1.14 times here: the preview keeps the
    // lattices of 1.9 and 4.3 pixels a cell whole, where a box would soften
    // them a little).
    REQUIRE(deviationOf(reduced) < deviationOf(full));
    REQUIRE(std::abs(deviationOf(reduced) / deviationOf(boxed) - 1.0) <= 0.15);
}

TEST_CASE("A smaller render samples the same lattices, only more sparsely",
          "[effects][grain][preview]") {
    // What "never moves what it can show" means for the model: output pixel x of a render
    // at a quarter of the size covers full-size pixels 4x to 4x + 3, so its
    // centre is at full-size pixel 4x + 1.5 on every lattice. Placed apart,
    // in double, the two agree to float rounding.
    const GrainPlan plan = planOf(100.0F, 100.0F, 100.0F, 3U);
    const GrainPlacement full = grainPlacementOf(plan, cornerOf(2048.0));
    const GrainPlacement reduced = grainPlacementOf(plan, cornerOf(512.0));
    const auto position = [](const GrainLayer& layer, std::size_t axis, double pixel) {
        return layer.cell[axis] + static_cast<double>(layer.fraction[axis]) +
               pixel * layer.delta[axis];
    };
    for (std::size_t index = 0; index < valueNoiseLatticeCount; ++index) {
        CAPTURE(index);
        REQUIRE(full.layers[index].seed == reduced.layers[index].seed);
        for (const double pixel : {0.0, 1.0, 37.0, 511.0}) {
            for (std::size_t axis = 0; axis < 2; ++axis) {
                REQUIRE(std::abs(position(reduced.layers[index], axis, pixel) -
                                 position(full.layers[index], axis, 4.0 * pixel + 1.5)) <= 1e-4);
            }
        }
        // Softer, never stronger.
        REQUIRE(reduced.layers[index].weight <= full.layers[index].weight);
    }
    REQUIRE(reduced.layers[0].weight == 0.0F);
    REQUIRE(reduced.layers[2].weight == full.layers[2].weight);
    // The substitute is the one layer that is the render's own: on the crop
    // frame, but at the render's scale, and only where something was left out.
    REQUIRE(full.layers[valueNoiseSubstituteLayer].weight == 0.0F);
    REQUIRE(reduced.layers[valueNoiseSubstituteLayer].weight > 0.0F);
    REQUIRE(reduced.layers[valueNoiseSubstituteLayer].delta[0] == 0.5F);
}

TEST_CASE("The substitute stays on the crop frame as a render pans", "[effects][grain][band]") {
    // Two renders of a 1024-pixel frame at the same scale, one panned by 37
    // pixels: where they overlap, the grain is the same, substitute and all.
    const GrainPlan plan = planOf(100.0F, 20.0F, 30.0F, 5U);
    REQUIRE(grainPlacementOf(plan, cornerOf(1024.0)).layers[valueNoiseSubstituteLayer].weight >
            0.0F);
    const Field still = grainOf(plan, cornerOf(1024.0), {256, 256});
    FrameMapping panned = cornerOf(1024.0);
    panned.origin = {37.0 / 1024.0, 37.0 / 1024.0};
    const Field moved = grainOf(plan, panned, {128, 128});
    REQUIRE(worstDifference(moved, still, 37, 37) <= regionTolerance);
}

TEST_CASE("A grain edit resumes from the resize, and the effects checkpoint is its seed's",
          "[effects][grain][checkpoint]") {
    const ImageBuffer source = greyOf({40, 30});
    const RenderRequest request{.size = RenderRequest::FitInside{24, 24}};
    const DevelopState before{grainy(30.0F, 50.0F, 50.0F, 1U)};
    const DevelopState after{grainy(70.0F, 80.0F, 20.0F, 1U)};
    const DevelopState reseeded{grainy(30.0F, 50.0F, 50.0F, 2U)};

    const auto resized = developUntil(source, before, Stage::Resize, request);
    const auto resumed = resumeFrom(resized, source, after, Stage::Effects, request);
    requireIdentical(develop(source, after, request), resumed.readBack());

    const auto effects = developUntil(source, before, Stage::Effects, request);
    REQUIRE_NOTHROW(resumeFrom(effects, source, before, Stage::Effects, request));
    REQUIRE_THROWS_AS(resumeFrom(effects, source, after, Stage::Effects, request),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(resumeFrom(effects, source, reseeded, Stage::Effects, request),
                      std::invalid_argument);
}

TEST_CASE("Grain is monochrome, follows the vignette and keeps alpha", "[effects][grain]") {
    DevelopSettings settings = grainy(70.0F, 100.0F, 30.0F, 11U);
    settings.effects.vignette = {-60.0F, 40.0F, 50.0F};
    ImageBuffer source = greyOf({48, 32});
    const auto input = source.samples<float>();
    for (std::size_t index = 3; index < input.size(); index += 8) {
        input[index] = 0.25F;
    }
    const ImageBuffer out = develop(source, DevelopState{settings});
    DevelopSettings vignetteOnly = settings;
    vignetteOnly.effects.grain = {};
    const ImageBuffer vignetted = develop(source, DevelopState{vignetteOnly});
    const auto got = out.samples<float>();
    const auto base = vignetted.samples<float>();
    for (std::size_t index = 0; index < got.size(); index += 4) {
        // The same grain on every channel of a grey, added to the vignetted value.
        REQUIRE(got[index] == got[index + 1]);
        REQUIRE(got[index] == got[index + 2]);
        REQUIRE(got[index + 3] == input[index + 3]);
        const float added = toPerceptualSigned(got[index]) - toPerceptualSigned(base[index]);
        REQUIRE(std::abs(added) <= 6.0F * strongestGrainDeviation);
    }
    // The order lives in effectsPixel: grain on the vignetted colour.
    const EffectsPlan plan = effectsPlanFor(settings.effects);
    const EffectsPlacement placement = effectsPlacementOf(plan, cornerOf(48.0));
    const Colour colour{0.3F, 0.3F, 0.3F};
    const FramePoint point = framePointOf(placement.mapping, 2, 3);
    const Colour expected =
        applyGrain(applyVignette(plan.vignette, colour, vignetteWeight(plan.vignette, point)),
                   grainAt(plan.grain, placement.grain, 2, 3));
    REQUIRE(effectsPixel(plan, placement, 2, 3, colour) == expected);
}
