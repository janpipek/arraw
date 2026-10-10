// Brush masks in a CPU render: the direct path (brush plan sections 2.5, 2.7 and 8.1 to 8.4).

#include "BrushCoverage.h"
#include "LadderAccess.h"
#include "LocalPlan.h"
#include "ProcessingPlan.h"
#include "ProgressScope.h"
#include "support/BrushGenerators.h"
#include "support/LadderTesting.h"

#include <CheckpointLadder.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <Progress.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::detail;
using namespace arraw::test;

namespace {

/// A smooth scene, different at every pixel.
ImageBuffer sceneOf(ImageSize size) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double shapes = 0.5 * std::sin(x / 37.0) * std::cos(y / 23.0) +
                                  0.3 * std::sin((x + y) / 61.0) +
                                  0.05 * std::sin(x / 2.3) * std::sin(y / 3.1);
            const auto base = static_cast<float>(0.18 * std::exp2(shapes));
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = base * 1.2F;
            pixel[1] = base;
            pixel[2] = base * 0.7F;
            pixel[3] = 1.0F;
        }
    }
    return image;
}

/// A flat grey image.
ImageBuffer flat(ImageSize size, float value = 0.2F) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    std::ranges::fill(image.samples<float>(), value);
    return image;
}

DevelopSettings plainSettings() {
    DevelopSettings settings;
    settings.tone.filmicHighlights = 0.0F;
    return settings;
}

DevelopState withMask(DevelopState state, Mask shape, const LocalDeltas& deltas,
                      bool invert = false) {
    LocalAdjustment adjustment;
    adjustment.shape = std::move(shape);
    adjustment.deltas = deltas;
    adjustment.invert = invert;
    return withLocalAdjustmentAdded(std::move(state), std::move(adjustment));
}

BrushMask brushOf(std::vector<Stroke> strokes) {
    return BrushMask{std::make_shared<const StrokeList>(std::move(strokes))};
}

/// A stroke along a horizontal line at v = 0.5, through the given corners.
Stroke horizontal(const std::vector<float>& us, float radius = 0.0625F, float hardness = 0.5F) {
    Stroke stroke{radius, hardness, 1.0F, false, {}};
    for (const float u : us) {
        stroke.points.push_back({u, 0.5F});
    }
    return stroke;
}

/// Splits every segment at its midpoint, `times` times over.
std::vector<float> split(std::vector<float> us, int times) {
    for (int t = 0; t < times; ++t) {
        std::vector<float> finer;
        for (std::size_t i = 0; i + 1 < us.size(); ++i) {
            finer.push_back(us[i]);
            finer.push_back((us[i] + us[i + 1]) / 2.0F);
        }
        finer.push_back(us.back());
        us = std::move(finer);
    }
    return us;
}

/// The pixel of a working-format image.
Colour pixelOf(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto samples = image.samples<float>();
    const float* pixel = &samples[(static_cast<std::size_t>(y) * image.size().width + x) * 4];
    return {pixel[0], pixel[1], pixel[2]};
}

bool sameColour(const Colour& a, const Colour& b) {
    return a == b;
}

/// Largest channel difference between two images of one size.
float largestDifference(const ImageBuffer& first, const ImageBuffer& second) {
    REQUIRE(first.size() == second.size());
    const auto a = first.samples<float>();
    const auto b = second.samples<float>();
    float largest = 0.0F;
    for (std::size_t index = 0; index < a.size(); ++index) {
        largest = std::max(largest, std::abs(a[index] - b[index]));
    }
    return largest;
}

/// The packed coverage a render of the state would read, at a source's size.
PackedCoverage packedFor(const DevelopState& state, ImageSize size) {
    return packCoverage(localPlanFor(state, size));
}

} // namespace

TEST_CASE("Outside every stroke a brush render is the render without masks", "[brush][render]") {
    const ImageSize size{128, 96};
    const ImageBuffer source = sceneOf(size);
    const DevelopState without{plainSettings()};
    const Stroke corner = test::straightStroke({0.1F, 0.15F}, {0.3F, 0.3F}, 6, 0.05F, 0.5F, 1.0F);
    const DevelopState brushed =
        withMask(without, brushOf({corner}), {.exposure = 1.0F, .saturation = 30.0F});
    const ImageBuffer reference = develop(source, without);
    const ImageBuffer masked = develop(source, brushed);
    const PackedCoverage packed = packedFor(brushed, size);

    std::size_t outside = 0;
    std::size_t inside = 0;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            if (packed.code(0, x, y) == 0) {
                REQUIRE(sameColour(pixelOf(masked, x, y), pixelOf(reference, x, y)));
                ++outside;
            } else if (!sameColour(pixelOf(masked, x, y), pixelOf(reference, x, y))) {
                ++inside;
            }
        }
    }
    REQUIRE(outside > 8000);
    REQUIRE(inside > 100);
}

TEST_CASE("An empty brush does nothing, and inverted it is the global render at g + k",
          "[brush][render]") {
    const ImageSize size{40, 30};
    const ImageBuffer source = sceneOf(size);
    DevelopSettings settings = plainSettings();
    settings.tone.exposure = 0.4F;
    const DevelopState global{settings};
    const ImageBuffer reference = develop(source, global);
    const DevelopState empty = withMask(global, brushOf({}), {.exposure = 1.5F});
    REQUIRE(sameBits(develop(source, empty), reference));

    const DevelopState inverted = withMask(global, brushOf({}), {.exposure = 1.5F}, true);
    DevelopSettings summed = plainSettings();
    summed.tone.exposure = 1.9F;
    REQUIRE(largestDifference(develop(source, inverted), develop(source, DevelopState{summed})) <=
            2e-6F);
}

TEST_CASE("A brush, a linear and a radial mask sum as the ADR says", "[brush][render]") {
    const ImageSize size{96, 64};
    const ImageBuffer source = flat(size);
    const auto brush =
        brushOf({test::straightStroke({0.2F, 0.5F}, {0.8F, 0.5F}, 8, 0.12F, 0.3F, 0.7F)});
    const LinearMask linear{{0.1F, 0.2F}, {0.9F, 0.7F}};
    const RadialMask radial{
        .centre = {0.4F, 0.6F}, .radiusX = 0.35F, .radiusY = 0.2F, .angle = 25.0F, .feather = 0.4F};
    DevelopState state{plainSettings()};
    state = withMask(state, linear, {.exposure = 1.0F});
    state = withMask(state, brush, {.exposure = 0.5F});
    state = withMask(state, radial, {.exposure = -0.75F}, true);
    const ImageBuffer out = develop(source, state);

    const LocalPlan plan = localPlanFor(state, size);
    const PackedCoverage packed = packCoverage(plan);
    REQUIRE(plan.masks.size() == 3);
    std::size_t reached = 0;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const auto px = static_cast<float>(x) + 0.5F;
            const auto py = static_cast<float>(y) + 0.5F;
            const float code = static_cast<float>(packed.code(plan.masks[1].brush.slot, x, y));
            reached += code > 0.0F ? 1U : 0U;
            const double stops = 1.0 * maskWeight(plan.masks[0], px, py, PixelCoverage{}) +
                                 0.5 * (static_cast<double>(code) / 255.0) -
                                 0.75 * maskWeight(plan.masks[2], px, py, PixelCoverage{});
            const double expected = 0.2 * std::exp2(stops);
            REQUIRE(pixelOf(out, x, y)[1] == Catch::Approx(expected).epsilon(3e-5).margin(1e-6));
        }
    }
    REQUIRE(reached > 500);
}

TEST_CASE("The same path at different event rates is the same render", "[brush][render][rates]") {
    // Dyadic corners, a power-of-two radius and even sides: every division in the dab placement
    // is exact, so splitting a segment at its midpoint changes no bit.
    for (const ImageSize size : {ImageSize{256, 192}, ImageSize{128, 96}}) {
        INFO(size.width << "x" << size.height);
        const ImageBuffer source = sceneOf(size);
        const std::vector<float> corners{0.25F, 0.5F, 0.75F};
        const auto render = [&](int splits) {
            const DevelopState state = withMask(DevelopState{plainSettings()},
                                                brushOf({horizontal(split(corners, splits))}),
                                                {.exposure = 1.0F, .contrast = 20.0F});
            return develop(source, state);
        };
        const ImageBuffer once = render(0);
        for (const int splits : {1, 2, 3}) {
            INFO(splits << " splits");
            REQUIRE(sameBits(render(splits), once));
        }
        // Through a ladder too (it uses the direct path for now).
        const auto shared = std::make_shared<const ImageBuffer>(sceneOf(size));
        const DevelopState state =
            withMask(DevelopState{plainSettings()}, brushOf({horizontal(split(corners, 2))}),
                     {.exposure = 1.0F, .contrast = 20.0F});
        CheckpointLadder ladder;
        REQUIRE(sameBits(resumeOrDevelop(ladder, shared, state, {}).checkpoint.readBack(), once));
    }
}

TEST_CASE("A diagonal path at 1x and 3x differs by one code at most, in few pixels",
          "[brush][render][rates]") {
    const ImageSize size{320, 240};
    const ImageBuffer source = sceneOf(size);
    const std::vector<SensorPoint> corners{{0.1F, 0.1F}, {0.8F, 0.3F}, {0.3F, 0.9F}};
    const auto path = [&](int perSegment) {
        Stroke stroke{0.04F, 0.5F, 0.8F, false, {}};
        for (std::size_t i = 0; i + 1 < corners.size(); ++i) {
            for (int n = 0; n < perSegment; ++n) {
                const float t = static_cast<float>(n) / static_cast<float>(perSegment);
                stroke.points.push_back({corners[i].u + (corners[i + 1].u - corners[i].u) * t,
                                         corners[i].v + (corners[i + 1].v - corners[i].v) * t});
            }
        }
        stroke.points.push_back(corners.back());
        return stroke;
    };
    const auto stateOf = [&](int perSegment) {
        return withMask(DevelopState{plainSettings()}, brushOf({path(perSegment)}),
                        {.exposure = 1.0F});
    };
    const DevelopState one = stateOf(1);
    const DevelopState three = stateOf(3);
    const PackedCoverage a = packedFor(one, size);
    const PackedCoverage b = packedFor(three, size);
    const ImageBuffer renderOne = develop(source, one);
    const ImageBuffer renderThree = develop(source, three);
    std::size_t reached = 0;
    std::size_t differing = 0;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const int ca = a.code(0, x, y);
            const int cb = b.code(0, x, y);
            if (ca == 0 && cb == 0) {
                continue;
            }
            ++reached;
            REQUIRE(std::abs(ca - cb) <= 1);
            if (ca != cb) {
                ++differing;
            } else {
                REQUIRE(sameColour(pixelOf(renderOne, x, y), pixelOf(renderThree, x, y)));
            }
        }
    }
    REQUIRE(reached > 3000);
    REQUIRE(differing * 100 <= reached);
}

TEST_CASE("A hardness-one stroke is the global render at g + k inside, and nothing outside, at "
          "every level",
          "[brush][render][levels]") {
    // The levels of an even source, a half-size decode of an odd one, and a tiny level.
    const std::vector<ImageSize> sizes{{256, 192}, {128, 96}, {64, 48}, {127, 95}, {33, 25}};
    for (const ImageSize size : sizes) {
        INFO(size.width << "x" << size.height);
        const ImageBuffer source = sceneOf(size);
        DevelopSettings settings = plainSettings();
        settings.tone.exposure = 0.25F;
        const DevelopState none{settings};
        DevelopSettings summed = plainSettings();
        summed.tone.exposure = 1.25F;
        // A fat stroke through the middle, a long-edge radius of a fifth.
        const DevelopState brushed = withMask(
            none, brushOf({test::straightStroke({0.3F, 0.5F}, {0.7F, 0.5F}, 12, 0.2F, 1.0F, 1.0F)}),
            {.exposure = 1.0F});
        const ImageBuffer rendered = develop(source, brushed);
        const ImageBuffer unmasked = develop(source, none);
        const ImageBuffer allIn = develop(source, DevelopState{summed});
        const PackedCoverage packed = packedFor(brushed, size);
        std::size_t deep = 0;
        std::size_t outside = 0;
        for (std::uint32_t y = 0; y < size.height; ++y) {
            for (std::uint32_t x = 0; x < size.width; ++x) {
                const std::uint8_t code = packed.code(0, x, y);
                if (code == 255) {
                    const Colour got = pixelOf(rendered, x, y);
                    const Colour want = pixelOf(allIn, x, y);
                    for (std::size_t c = 0; c < 3; ++c) {
                        REQUIRE(std::abs(got[c] - want[c]) <= 2e-6F);
                    }
                    ++deep;
                } else if (code == 0) {
                    REQUIRE(sameColour(pixelOf(rendered, x, y), pixelOf(unmasked, x, y)));
                    ++outside;
                }
            }
        }
        REQUIRE(deep > 0);
        REQUIRE(outside > 0);
    }
}

TEST_CASE("A ladder render with a brush equals the direct render", "[brush][render][ladder]") {
    const auto source = std::make_shared<const ImageBuffer>(sceneOf({96, 64}));
    DevelopState state{plainSettings()};
    state = withMask(
        state, brushOf({test::straightStroke({0.2F, 0.3F}, {0.7F, 0.6F}, 10, 0.08F, 0.4F, 0.9F)}),
        {.exposure = 0.8F, .shadows = 30.0F});
    CheckpointLadder ladder;
    const ImageBuffer first = resumeOrDevelop(ladder, source, state, {}).checkpoint.readBack();
    REQUIRE(sameBits(first, develop(*source, state)));
    // A delta edit resumes from the held rung and still gives the direct render's bits.
    const DevelopState louder =
        withLocalDelta(state, state.localAdjustments[0].id, "exposure", 1.6);
    const LadderRender again = resumeOrDevelop(ladder, source, louder, {});
    REQUIRE(sameBits(again.checkpoint.readBack(), develop(*source, louder)));
}

TEST_CASE("A region render of a brush is the crop of the whole", "[brush][render][region]") {
    const ImageSize size{100, 80};
    const ImageBuffer source = sceneOf(size);
    DevelopSettings settings = plainSettings();
    settings.geometry.straighten = 3.0;
    const DevelopState state =
        withMask(DevelopState{settings},
                 brushOf({test::straightStroke({0.2F, 0.3F}, {0.8F, 0.7F}, 12, 0.1F, 0.5F, 1.0F)}),
                 {.exposure = 1.0F, .texture = 30.0F});
    const ImageBuffer whole = develop(source, state);
    const RenderRequest request{.region = RenderRequest::Region{0.25, 0.5, 0.75, 1.0}};
    const ImageBuffer part = develop(source, state, request);
    const PixelRegion pixels = regionOf(request, whole.size());
    REQUIRE(part.size() == pixels.size());
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        for (std::uint32_t x = 0; x < pixels.width; ++x) {
            REQUIRE(sameColour(pixelOf(part, x, y), pixelOf(whole, pixels.x + x, pixels.y + y)));
        }
    }
}

TEST_CASE("A cancelled render leaves the cache consistent and the next render correct",
          "[brush][render][cancel]") {
    brushCoverageCache().clear();
    const ImageSize size{400, 300};
    const ImageBuffer source = sceneOf(size);
    const auto list = paintedMask(77, 5, everydayStyle, 0.75);
    const DevelopState state =
        withMask(DevelopState{plainSettings()}, BrushMask{list}, {.exposure = 1.0F});
    const ImageBuffer reference = develop(source, state);
    brushCoverageCache().clear();

    SECTION("cancelled before it starts") {
        ProgressChannel channel;
        channel.cancel();
        REQUIRE_THROWS_AS(develop(source, state, {}, &channel), Cancelled);
    }
    SECTION("cancelled by its first report") {
        ProgressChannel channel([&channel](const Progress&) { channel.cancel(); });
        REQUIRE_THROWS_AS(develop(source, state, {}, &channel), Cancelled);
    }
    SECTION("cancelled while the coverage is packed") {
        const LocalPlan plan = localPlanFor(state, size);
        ProgressChannel channel;
        StepWeights weights{};
        weights[static_cast<std::size_t>(ProgressStep::Pointwise)] = 1.0;
        ProgressRoot root(&channel, weights, ProgressStep::Pointwise);
        channel.cancel();
        REQUIRE_THROWS_AS(packCoverage(plan), Cancelled);
        // A cancelled draw inserts nothing.
        REQUIRE(brushCoverageCache().entryCount() == 0);
    }
    // Whatever the cache holds is the reference's, and the next render is right.
    if (const auto held = brushCoverageCache().find(list, size)) {
        REQUIRE(held->gathered() == rasteriseBrush(*list, size));
    }
    REQUIRE(sameBits(develop(source, state), reference));
}

TEST_CASE("Thumbnail-sized develop and sample work with brushes", "[brush][render]") {
    const ImageBuffer source = sceneOf({300, 200});
    const DevelopState state =
        withMask(DevelopState{plainSettings()},
                 brushOf({test::straightStroke({0.2F, 0.3F}, {0.8F, 0.7F}, 12, 0.1F, 0.5F, 1.0F)}),
                 {.exposure = 1.0F, .saturation = 20.0F});
    const RenderRequest thumbnail{.size = RenderRequest::FitInside{40, 40}};
    const ImageBuffer small = develop(source, state, thumbnail);
    REQUIRE(small.size() == ImageSize{40, 27});
    const ImageBuffer tap = sample(source, state, Tap::CurveInput, thumbnail);
    REQUIRE(tap.size() == small.size());
    // Another size of the same strokes is another raster, and brightens where the brush is.
    const DevelopState none{plainSettings()};
    REQUIRE_FALSE(sameBits(small, develop(source, none, thumbnail)));
}

TEST_CASE("A brush the plan keeps still renders through the tap's prefix", "[brush][render][tap]") {
    const ImageBuffer source = sceneOf({64, 48});
    const DevelopState none{plainSettings()};
    const DevelopState lit = withMask(
        none, brushOf({test::straightStroke({0.2F, 0.5F}, {0.8F, 0.5F}, 8, 0.2F, 1.0F, 1.0F)}),
        {.exposure = 1.0F});
    const ImageBuffer tap = sample(source, lit, Tap::CurveInput);
    const ImageBuffer plain = sample(source, none, Tap::CurveInput);
    // The middle of the stroke is brighter, a corner is not.
    REQUIRE(pixelOf(tap, 32, 24)[1] > pixelOf(plain, 32, 24)[1]);
    REQUIRE(sameColour(pixelOf(tap, 1, 1), pixelOf(plain, 1, 1)));
}

TEST_CASE("developUntil and resumeFrom carry a brush to the direct render's bits",
          "[brush][render][checkpoint]") {
    const ImageBuffer source = sceneOf({96, 64});
    const DevelopState state =
        withMask(DevelopState{plainSettings()},
                 brushOf({test::straightStroke({0.2F, 0.3F}, {0.7F, 0.6F}, 10, 0.08F, 0.4F, 0.9F)}),
                 {.exposure = 0.8F, .shadows = 30.0F});
    const ImageBuffer direct = develop(source, state);

    const RenderCheckpoint upTo = developUntil(source, state, Stage::Denoise);
    const RenderCheckpoint finished = resumeFrom(upTo, source, state, Stage::Effects);
    REQUIRE(sameBits(finished.readBack(), direct));
    REQUIRE(sameBits(developUntil(source, state, Stage::Effects).readBack(), direct));
    // An edit of the brush's amount resumes from the same checkpoint to its own direct render.
    const DevelopState louder =
        withLocalDelta(state, state.localAdjustments[0].id, "exposure", 1.6);
    REQUIRE(sameBits(resumeFrom(upTo, source, louder, Stage::Effects).readBack(),
                     develop(source, louder)));
}

TEST_CASE("The progress of a brush render never decreases", "[brush][render][progress]") {
    brushCoverageCache().clear();
    const ImageBuffer source = sceneOf({400, 300});
    const DevelopState state =
        withMask(DevelopState{plainSettings()}, BrushMask{paintedMask(77, 5, everydayStyle, 0.75)},
                 {.exposure = 1.0F});
    std::vector<double> fractions;
    ProgressChannel channel(
        [&fractions](const Progress& progress) { fractions.push_back(progress.fraction); });
    (void)develop(source, state, {}, &channel);
    REQUIRE_FALSE(fractions.empty());
    REQUIRE(std::ranges::is_sorted(fractions));
    REQUIRE(fractions.back() <= 1.0);
}

TEST_CASE("A delta drag through a ladder does no coverage work", "[brush][render][ladder]") {
    brushCoverageCache().clear();
    const auto source = std::make_shared<const ImageBuffer>(sceneOf({160, 120}));
    DevelopState state{plainSettings()};
    state = withMask(state, BrushMask{paintedMask(31, 4, everydayStyle, 0.75)},
                     {.exposure = 0.8F, .shadows = 30.0F});
    state = withMask(
        state, brushOf({test::straightStroke({0.2F, 0.3F}, {0.7F, 0.6F}, 10, 0.08F, 0.4F, 0.9F)}),
        {.exposure = -0.4F});
    CheckpointLadder ladder;
    REQUIRE(sameBits(resumeOrDevelop(ladder, source, state, {}).checkpoint.readBack(),
                     develop(*source, state)));
    const CoverageResidency& residency = *LadderAccess::coverage(std::as_const(ladder));
    const std::uint64_t calls = residency.cacheCalls();
    const std::uint64_t packed = residency.tilesPacked();
    REQUIRE(calls == 2);

    DevelopState dragged = state;
    for (const double amount : {1.0, 1.4, 1.9, -0.5}) {
        dragged = withLocalDelta(dragged, dragged.localAdjustments[0].id, "exposure", amount);
        const LadderRender render = resumeOrDevelop(ladder, source, dragged, {});
        REQUIRE(sameBits(render.checkpoint.readBack(), develop(*source, dragged)));
    }
    REQUIRE(residency.cacheCalls() == calls);
    REQUIRE(residency.tilesPacked() == packed);
}

TEST_CASE("A ladder render follows an appended stroke, an undo and a disabled brush bit for bit",
          "[brush][render][ladder]") {
    brushCoverageCache().clear();
    const auto source = std::make_shared<const ImageBuffer>(sceneOf({300, 200}));
    DevelopState state{plainSettings()};
    state = withMask(state, BrushMask{paintedMask(32, 4, everydayStyle, 2.0 / 3.0)},
                     {.exposure = 0.8F});
    state =
        withMask(state, BrushMask{paintedMask(33, 4, detailStyle, 2.0 / 3.0)}, {.shadows = 40.0F});
    CheckpointLadder ladder;
    const auto render = [&](const DevelopState& wanted) {
        const ImageBuffer ladderBits =
            resumeOrDevelop(ladder, source, wanted, {}).checkpoint.readBack();
        REQUIRE(sameBits(ladderBits, develop(*source, wanted)));
    };
    render(state);
    const CoverageResidency& residency = *LadderAccess::coverage(std::as_const(ladder));
    const std::uint64_t packed = residency.tilesPacked();

    const DevelopState more = withStrokeAppended(
        state, state.localAdjustments[1].id,
        test::straightStroke({0.05F, 0.05F}, {0.1F, 0.1F}, 5, 0.02F, 1.0F, 1.0F));
    render(more);
    const std::uint64_t repacked = residency.tilesPacked() - packed;
    REQUIRE(repacked > 0);
    REQUIRE(repacked < 3 * 2); // Fewer than all six tiles of the one brush.
    // Undo: the earlier list again, and the tiles that differ only.
    render(state);
    REQUIRE(residency.tilesPacked() - packed == 2 * repacked);

    // An earlier brush disabled: the later one moves to slot 0, repacked from the cache.
    const std::size_t entries = brushCoverageCache().entryCount();
    render(withLocalAdjustmentEnabled(state, state.localAdjustments[0].id, false));
    REQUIRE(brushCoverageCache().entryCount() == entries);
    render(state);
}

TEST_CASE("Another source or size starts the ladder's coverage again", "[brush][render][ladder]") {
    brushCoverageCache().clear();
    DevelopState state{plainSettings()};
    state = withMask(state, BrushMask{paintedMask(34, 4, everydayStyle, 2.0 / 3.0)},
                     {.exposure = 0.8F});
    CheckpointLadder ladder;
    for (const ImageSize size : {ImageSize{240, 160}, ImageSize{120, 80}, ImageSize{240, 160}}) {
        const auto source = std::make_shared<const ImageBuffer>(sceneOf(size));
        REQUIRE(sameBits(resumeOrDevelop(ladder, source, state, {}).checkpoint.readBack(),
                         develop(*source, state)));
    }
}

TEST_CASE("The Coverage step is reported between Context and Pointwise, and never falls",
          "[brush][render][progress]") {
    brushCoverageCache().clear();
    const auto source = std::make_shared<const ImageBuffer>(sceneOf({400, 300}));
    DevelopState state{plainSettings()};
    state.settings.presence.clarity = 30.0F;
    state = withMask(state, BrushMask{paintedMask(77, 5, everydayStyle, 0.75)}, {.exposure = 1.0F});

    ReportLog log;
    ProgressChannel channel(log.callback());
    static_cast<void>(develop(*source, state, {}, &channel));
    REQUIRE(log.wellFormed());
    std::vector<ProgressStep> steps;
    for (const Progress& report : log.reports) {
        if (steps.empty() || steps.back() != report.step) {
            steps.push_back(report.step);
        }
    }
    const auto at = [&](ProgressStep step) { return std::ranges::find(steps, step); };
    REQUIRE(at(ProgressStep::Coverage) != steps.end());
    REQUIRE(at(ProgressStep::Context) < at(ProgressStep::Coverage));
    REQUIRE(at(ProgressStep::Coverage) < at(ProgressStep::Pointwise));

    // Through a ladder: the first render paints, a drag of the amount reports no Coverage.
    CheckpointLadder ladder;
    ReportLog first;
    ProgressChannel firstChannel(first.callback());
    static_cast<void>(resumeOrDevelop(ladder, source, state, {}, &firstChannel));
    REQUIRE(first.wellFormed());
    REQUIRE(std::ranges::any_of(first.reports, [](const Progress& report) {
        return report.step == ProgressStep::Coverage;
    }));
    ReportLog drag;
    ProgressChannel dragChannel(drag.callback());
    static_cast<void>(resumeOrDevelop(
        ladder, source, withLocalDelta(state, state.localAdjustments[0].id, "exposure", 2.0), {},
        &dragChannel));
    REQUIRE(drag.wellFormed());
    REQUIRE(std::ranges::none_of(drag.reports, [](const Progress& report) {
        return report.step == ProgressStep::Coverage;
    }));
}

TEST_CASE("A ladder render cancelled while its coverage is made leaves the next one right",
          "[brush][render][ladder][cancel]") {
    brushCoverageCache().clear();
    const auto source = std::make_shared<const ImageBuffer>(sceneOf({400, 300}));
    DevelopState state{plainSettings()};
    state = withMask(state, BrushMask{paintedMask(78, 5, everydayStyle, 0.75)}, {.exposure = 1.0F});
    state = withMask(state, BrushMask{paintedMask(79, 5, detailStyle, 0.75)}, {.shadows = 30.0F});
    const ImageBuffer reference = develop(*source, state);
    brushCoverageCache().clear();

    CheckpointLadder ladder;
    for (const int cancelAt : {1, 2, 3, 6}) {
        INFO("cancelled at report " << cancelAt);
        int seen = 0;
        ProgressChannel channel([&](const Progress& progress) {
            if (progress.step == ProgressStep::Coverage && ++seen >= cancelAt) {
                channel.cancel();
            }
        });
        try {
            static_cast<void>(resumeOrDevelop(ladder, source, state, {}, &channel));
        } catch (const Cancelled&) {
            // Either way, nothing wrong is kept.
        }
        // The cache holds only the reference's coverage.
        for (const auto& mask : localPlanFor(state, source->size()).masks) {
            if (const auto held = brushCoverageCache().find(mask.brush.strokes, source->size())) {
                REQUIRE(held->gathered() == rasteriseBrush(*mask.brush.strokes, source->size()));
            }
        }
        REQUIRE(
            sameBits(resumeOrDevelop(ladder, source, state, {}).checkpoint.readBack(), reference));
        ladder.clear();
        brushCoverageCache().clear();
    }
}
