#include "BrushCoverage.h"
#include "GpuContext.h"
#include "GpuCoverage.h"
#include "GpuDevelop.h"
#include "GpuPlan.h"
#include "GpuTesting.h"
#include "LadderAccess.h"
#include "LocalPlan.h"
#include "ProcessingPlan.h"
#include "SampleConversion.h"
#include "support/BrushGenerators.h"
#include "support/LadderTesting.h"
#include "support/LocalAdjustmentStates.h"
#include "support/RenderDigest.h"

#include <BrushStrokes.h>
#include <CheckpointLadder.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImagePyramid.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <Progress.h>
#include <RenderCheckpoint.h>
#include <SettingDescriptors.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::detail;
using namespace arraw::test;

/// Brush masks on the device against the CPU (brush plan sections 4 and 8.5, ADR 044 section 7).
///
/// Coverage is packed on the host and read by the shader as texels, so the texels are tested for
/// equality and the renders against the CPU's within the pointwise tolerance.

namespace {

/// @brief Largest error of the pointwise pass under a brush that is not the CPU's (ADR 041).
constexpr double brushRelativeTolerance = 3e-5;

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

/// @brief A smooth scene with a gradient, for the larger sources of the ladder tests.
ImageBuffer gentleOf(ImageSize size) {
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

DevelopSettings plainSettings() {
    DevelopSettings settings;
    settings.tone.filmicHighlights = 0.0F;
    return settings;
}

BrushMask brushOf(std::vector<Stroke> strokes) {
    return BrushMask{std::make_shared<const StrokeList>(std::move(strokes))};
}

/// @brief Builds an adjustment of a shape with some deltas, by key.
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

/// @brief A soft, wide stroke through the middle of the frame.
BrushMask middleBrush(bool erase = false) {
    std::vector<Stroke> strokes;
    strokes.push_back(straightStroke({0.15F, 0.35F}, {0.85F, 0.65F}, 14, 0.14F, 0.2F, 0.9F));
    if (erase) {
        strokes.push_back(straightStroke({0.5F, 0.2F}, {0.5F, 0.8F}, 8, 0.07F, 0.6F, 1.0F, true));
    }
    return brushOf(std::move(strokes));
}

/// @brief A linear mask that is one everywhere in the frame.
LinearMask everywhere() {
    return {.from = {0.5F, 2.0F}, .to = {0.5F, 2.5F}};
}

/// @brief A delta that does something visible for a control.
double visibleDelta(const LocalDescriptor& descriptor) {
    return descriptor.range.maximum * 0.6;
}

/// @brief Requires the device's pointwise boundary to match the CPU's under the state's masks.
/// @return The worst error, relative to the pixel's scale.
double requirePointwiseMatches(const ImageBuffer& source, const DevelopState& state,
                               double tolerance = brushRelativeTolerance) {
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
        std::fprintf(stderr, "brush pointwise: effect %.3g, error %.3g\n", effect, error);
    }
    CAPTURE(effect, error, tolerance, compareFloat(expected, actual, pointwiseAbsoluteFloor));
    // The masks did something, far more than the bound.
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

/// @brief The pixel of a working-format image.
std::array<float, 3> pixelOf(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto samples = image.samples<float>();
    const float* pixel = &samples[(static_cast<std::size_t>(y) * image.size().width + x) * 4];
    return {pixel[0], pixel[1], pixel[2]};
}

/// @brief Random bytes, every value of a code among them.
std::vector<std::uint8_t> randomPlane(ImageSize size, std::uint64_t seed) {
    std::mt19937_64 generator(seed);
    std::vector<std::uint8_t> plane(size.pixelCount() * 4);
    for (std::size_t index = 0; index < plane.size(); ++index) {
        // Every code in turn first, so that the whole range is certain to be there.
        plane[index] = index < 256 ? static_cast<std::uint8_t>(index)
                                   : static_cast<std::uint8_t>(generator() & 0xFFU);
    }
    return plane;
}

/// @brief Loads a fixture with its own encoding and orientation.
ImageBuffer fixtureImage(const char* name) {
    return loadImage(std::filesystem::path(ARRAW_TEST_DATA_DIR) / name);
}

/// @brief Reads a coverage texture back as bytes.
std::vector<std::uint8_t> bytesOfTexture(const DeviceImage& texture) {
    const ImageBuffer buffer = texture.readBack();
    REQUIRE(buffer.format() == PixelFormat::RgbaU8);
    const std::span<const std::byte> bytes = buffer.bytes();
    std::vector<std::uint8_t> result(bytes.size());
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

/// @brief Whether a texture reads back as the bytes (so that a failure prints no byte dump).
bool holdsBytes(const DeviceImage& texture, const std::vector<std::uint8_t>& bytes) {
    return bytesOfTexture(texture) == bytes;
}

} // namespace

TEST_CASE("A coverage texture reads back as its plane", "[gpu][brush][texture]") {
    GpuContext& context = gpuContext();
    for (const ImageSize size : {ImageSize{1, 1}, ImageSize{37, 29}, ImageSize{300, 201}}) {
        INFO(size.width << "x" << size.height);
        // Enough texels for every code to appear in each channel.
        const std::vector<std::uint8_t> plane = randomPlane(size, size.width * 31 + size.height);
        const DeviceImage texture = context.uploadCoverage(size, plane);
        REQUIRE(texture.valid());
        REQUIRE(texture.size() == size);
        REQUIRE(texture.format() == PixelFormat::RgbaU8);
        REQUIRE(texture.pixelScale() == 1.0);
        // Equal bytes read back is the storage; that the codes are not linearised on sampling is
        // shown by the renders against the CPU below.
        REQUIRE(holdsBytes(texture, plane));
    }
    SECTION("a plane of the wrong length, or of no size, is refused") {
        REQUIRE_THROWS_AS(context.uploadCoverage({4, 4}, std::vector<std::uint8_t>(63)),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(context.uploadCoverage({0, 4}, std::vector<std::uint8_t>()),
                          std::invalid_argument);
    }
}

TEST_CASE("Updating rectangles of a coverage texture equals uploading the plane whole",
          "[gpu][brush][texture]") {
    GpuContext& context = gpuContext();
    const ImageSize size{301, 197};
    const std::vector<std::uint8_t> before = randomPlane(size, 5);
    std::vector<std::uint8_t> after = before;
    // Rectangles at the corners, one a single texel, one along an edge, and an inner tile.
    const std::vector<PixelRect> rectangles{{0, 0, 128, 128},  {256, 128, 45, 69},
                                            {300, 196, 1, 1},  {128, 69, 128, 128},
                                            {0, 128, 128, 69}, {40, 190, 200, 7}};
    std::mt19937_64 generator(9);
    for (const PixelRect& rect : rectangles) {
        for (std::uint32_t y = rect.y; y < rect.y + rect.height; ++y) {
            for (std::uint32_t x = rect.x; x < rect.x + rect.width; ++x) {
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    after[(static_cast<std::size_t>(y) * size.width + x) * 4 + channel] =
                        static_cast<std::uint8_t>(generator() & 0xFFU);
                }
            }
        }
    }
    const DeviceImage texture = context.uploadCoverage(size, before);
    context.updateCoverage(texture, rectangles, after);
    REQUIRE(holdsBytes(texture, after));
    REQUIRE(holdsBytes(context.uploadCoverage(size, after), after));

    SECTION("no rectangles change nothing") {
        const DeviceImage fresh = context.uploadCoverage(size, before);
        context.updateCoverage(fresh, {}, after);
        REQUIRE(holdsBytes(fresh, before));
    }
    SECTION("a rectangle outside the texture, an empty one, or a plane of another size throws") {
        const DeviceImage fresh = context.uploadCoverage(size, before);
        REQUIRE_THROWS_AS(
            context.updateCoverage(fresh, std::vector<PixelRect>{{250, 0, 60, 4}}, before),
            std::invalid_argument);
        REQUIRE_THROWS_AS(
            context.updateCoverage(fresh, std::vector<PixelRect>{{0, 0, 0, 4}}, before),
            std::invalid_argument);
        REQUIRE_THROWS_AS(context.updateCoverage(fresh, std::vector<PixelRect>{{0, 0, 1, 1}},
                                                 std::vector<std::uint8_t>(16)),
                          std::invalid_argument);
        REQUIRE(holdsBytes(fresh, before));
    }
    SECTION("only a coverage texture can be updated") {
        const DeviceImage colour = context.upload(gentleOf({8, 8}));
        REQUIRE_THROWS_AS(
            context.updateCoverage(colour, std::vector<PixelRect>{{0, 0, 1, 1}}, before),
            std::invalid_argument);
    }
}

TEST_CASE("Each control alone, on a brush, matches the CPU", "[gpu][brush]") {
    // An odd size, so that the last row and column are partial.
    const ImageBuffer source = sceneOf({131, 77});
    double worst = 0.0;
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        for (const double sign : {1.0, -1.0}) {
            INFO(descriptor.key << ", sign " << sign);
            const double delta = sign * visibleDelta(descriptor);
            const DevelopState state = stateOf(
                {adjustmentOf(middleBrush(), {{std::string(descriptor.key), delta}}, 0.9F)});
            worst = std::max(worst, requirePointwiseMatches(source, state));
        }
    }
    if (printsMeasured()) {
        std::fprintf(stderr, "brush controls: worst %.3g\n", worst);
    }
}

TEST_CASE("Every control together on a brush, with the globals they add to, matches the CPU",
          "[gpu][brush]") {
    const ImageBuffer source = sceneOf({131, 77});
    DevelopSettings settings;
    settings.tone = {0.3F, 20.0F, 15.0F, -20.0F, 10.0F, -10.0F, 0.0F};
    settings.color.saturation = 20.0F;
    settings.color.vibrance = -10.0F;
    settings.presence = {.texture = 20.0F, .clarity = -15.0F, .dehaze = 25.0F};
    DevelopState state = stateOf({adjustmentOf(middleBrush(), {})}, settings);
    LocalDeltas deltas;
    giveEveryDelta(deltas);
    state.localAdjustments[0].deltas = deltas;
    requirePointwiseMatches(source, state);
}

TEST_CASE("A brush with erase strokes, inverted, and an inverted empty brush match the CPU",
          "[gpu][brush]") {
    const ImageBuffer source = sceneOf({131, 77});
    const std::vector<std::pair<std::string, double>> deltas{
        {"exposure", 1.0}, {"shadows", 40.0}, {"saturation", 30.0}};
    SECTION("paint and erase") {
        requirePointwiseMatches(source, stateOf({adjustmentOf(middleBrush(true), deltas)}));
    }
    SECTION("inverted") {
        requirePointwiseMatches(source,
                                stateOf({adjustmentOf(middleBrush(true), deltas, 0.8F, true)}));
    }
    SECTION("an empty brush draws nothing") {
        const DevelopState state = stateOf({adjustmentOf(brushOf({}), deltas)});
        requireBitIdentical(
            developOnGpu(gpuContext(), source, withoutMasks(state), Stage::Pointwise).readBack(),
            developOnGpu(gpuContext(), source, state, Stage::Pointwise).readBack());
    }
    SECTION("an inverted empty brush is everywhere") {
        requirePointwiseMatches(source, stateOf({adjustmentOf(brushOf({}), deltas, 1.0F, true)}));
    }
}

TEST_CASE("Sixteen brushes, four textures and every channel, match the CPU", "[gpu][brush]") {
    const ImageSize size{131, 77};
    const ImageBuffer source = sceneOf(size);
    std::vector<LocalAdjustment> adjustments;
    std::mt19937_64 generator(2024);
    for (int index = 0; index < 16; ++index) {
        LocalAdjustment adjustment;
        adjustment.shape = BrushMask{
            paintedMask(100 + static_cast<std::uint64_t>(index), 2, everydayStyle, 77.0 / 131.0)};
        adjustment.invert = index % 7 == 3;
        adjustment.opacity = index % 3 == 0 ? 0.7F : 1.0F;
        // Every brush a different control and sign, so that a wrong slot shows.
        const LocalDescriptor& descriptor =
            localAdjustmentDescriptors[static_cast<std::size_t>(index) %
                                       localAdjustmentDescriptors.size()];
        adjustment.deltas.*descriptor.member =
            static_cast<float>((index % 2 == 0 ? 0.4 : -0.4) * visibleDelta(descriptor));
        adjustment.deltas.exposure += 0.05F * static_cast<float>(index % 5);
        adjustments.push_back(adjustment);
    }
    const DevelopState state = stateOf(adjustments);
    const ProcessingPlan plan = planFor(source, state);
    REQUIRE(plan.pointwise.local.brushCount() == 16);
    requirePointwiseMatches(source, state);
    // The last slot of the last texture reads its own channel: the other fifteen stay in the plan
    // with a faint exposure, so that the loud one keeps slot 15 and a wrong channel shows.
    std::vector<LocalAdjustment> faint = adjustments;
    for (std::size_t index = 0; index + 1 < faint.size(); ++index) {
        faint[index].deltas = {};
        faint[index].deltas.exposure = 0.01F;
    }
    const DevelopState last = stateOf(faint);
    REQUIRE(planFor(source, last).pointwise.local.brushCount() == 16);
    requirePointwiseMatches(source, last);
}

TEST_CASE("Brushes mixed with linear and radial masks match the CPU", "[gpu][brush]") {
    const ImageBuffer source = sceneOf({131, 77});
    DevelopSettings settings;
    settings.tone.exposure = 0.2F;
    settings.presence = {.texture = 10.0F, .clarity = 0.0F, .dehaze = 15.0F};
    const DevelopState state = stateOf(
        {adjustmentOf(someLinear(), {{"exposure", 0.8}, {"saturation", 30.0}}),
         adjustmentOf(middleBrush(true), {{"exposure", -0.5}, {"clarity", 40.0}}),
         adjustmentOf(someRadial(), {{"relativeTemperature", 60.0}, {"contrast", -30.0}}, 0.9F,
                      true),
         adjustmentOf(brushOf({straightStroke({0.2F, 0.2F}, {0.8F, 0.8F}, 9, 0.1F, 1.0F, 1.0F)}),
                      {{"dehaze", 50.0}, {"relativeTint", -40.0}}, 0.7F, true)},
        settings);
    requirePointwiseMatches(source, state);
}

TEST_CASE("Brush coverage sums with a cancelling mask as the CPU's, and exactly where it is one",
          "[gpu][brush]") {
    const ImageBuffer source = sceneOf({131, 77});
    DevelopSettings settings;
    settings.tone.exposure = 0.3F;
    settings.presence.clarity = 20.0F;
    // A hardness-one stroke is code 255 deep inside; a linear mask that is one everywhere with
    // the opposite deltas cancels there exactly, and the pixel takes the global path.
    const BrushMask hard =
        brushOf({straightStroke({0.2F, 0.5F}, {0.8F, 0.5F}, 10, 0.2F, 1.0F, 1.0F)});
    LocalDeltas deltas;
    giveEveryDelta(deltas);
    LocalDeltas opposite;
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        opposite.*descriptor.member = -(deltas.*descriptor.member);
    }
    LocalAdjustment brush;
    brush.shape = hard;
    brush.deltas = deltas;
    LocalAdjustment flat;
    flat.shape = everywhere();
    flat.deltas = opposite;
    const DevelopState state = stateOf({brush, flat}, settings);
    const ProcessingPlan plan = planFor(source, state);
    const PackedCoverage packed = packCoverage(plan.pointwise.local);

    const ImageBuffer without =
        developOnGpu(gpuContext(), source, withoutMasks(state), Stage::Pointwise).readBack();
    const ImageBuffer masked =
        developOnGpu(gpuContext(), source, state, Stage::Pointwise).readBack();
    std::size_t exact = 0;
    std::size_t elsewhere = 0;
    for (std::uint32_t y = 0; y < source.size().height; ++y) {
        for (std::uint32_t x = 0; x < source.size().width; ++x) {
            const std::uint8_t code = packed.code(plan.pointwise.local.masks[0].brush.slot, x, y);
            if (code == 255) {
                REQUIRE(pixelOf(masked, x, y) == pixelOf(without, x, y));
                ++exact;
            } else if (code == 0) {
                // The flat mask alone: its deltas against the global ones.
                ++elsewhere;
            }
        }
    }
    REQUIRE(exact > 200);
    REQUIRE(elsewhere > 200);
    requirePointwiseMatches(source, state);
}

TEST_CASE("Mixed-sign Dehaze on brushes matches the CPU", "[gpu][brush][presence]") {
    const ImageBuffer source = sceneOf({131, 77});
    for (const float global : {0.0F, 20.0F, -20.0F}) {
        INFO("global dehaze " << global);
        DevelopSettings settings;
        settings.presence = {.texture = 0.0F, .clarity = 0.0F, .dehaze = global};
        const DevelopState state = stateOf(
            {adjustmentOf(middleBrush(), {{"dehaze", 70.0}}),
             adjustmentOf(
                 brushOf({straightStroke({0.1F, 0.8F}, {0.9F, 0.2F}, 10, 0.12F, 0.3F, 1.0F)}),
                 {{"dehaze", -60.0}, {"texture", 40.0}})},
            settings);
        requirePointwiseMatches(source, state);
    }
}

TEST_CASE("Brush masks match the CPU at odd sizes", "[gpu][brush]") {
    for (const ImageSize size : {ImageSize{1, 1}, ImageSize{2, 3}, ImageSize{17, 5},
                                 ImageSize{5, 17}, ImageSize{97, 61}, ImageSize{61, 97}}) {
        INFO(size.width << "x" << size.height);
        const ImageBuffer source = sceneOf(size);
        const DevelopState state =
            stateOf({adjustmentOf(
                         brushOf({straightStroke({0.0F, 0.0F}, {1.0F, 1.0F}, 9, 0.5F, 0.5F, 1.0F)}),
                         {{"exposure", 1.5}, {"blacks", 40.0}}),
                     adjustmentOf(brushOf({}), {{"shadows", 50.0}}, 1.0F, true)});
        if (size.pixelCount() == 1) {
            // One pixel: both backends read the same code; compare the render, not the effect.
            const ImageBuffer expected = developUntil(source, state, Stage::Pointwise).readBack();
            const ImageBuffer actual =
                developOnGpu(gpuContext(), source, state, Stage::Pointwise).readBack();
            REQUIRE(worstColourError(expected, actual) <= brushRelativeTolerance);
            continue;
        }
        requirePointwiseMatches(source, state);
    }
}

TEST_CASE("A render with a brush matches the CPU at pyramid levels 0 to 3, in a region and "
          "through the geometry",
          "[gpu][brush][pyramid]") {
    GpuContext& context = gpuContext();
    DevelopSettings settings;
    settings.tone.exposure = 0.2F;
    settings.geometry.straighten = 4.0;
    settings.geometry.crop.rectangle = UprightCropRect{0.05, 0.1, 0.95, 0.9};
    const DevelopState state =
        stateOf({adjustmentOf(middleBrush(true),
                              {{"exposure", 0.8}, {"dehaze", 40.0}, {"saturation", 30.0}}),
                 adjustmentOf(someRadial(), {{"relativeTemperature", 60.0}, {"contrast", -30.0}},
                              0.9F, true)},
                settings);
    const ImageBuffer full = sceneOf({203, 141});
    std::vector<ImageBuffer> levels;
    levels.push_back(full.clone());
    for (int level = 1; level <= 3; ++level) {
        levels.push_back(halved(levels.back()));
    }
    const auto requireRender = [&](const ImageBuffer& source, const RenderRequest& request) {
        const ImageBuffer expected = develop(source, state, request);
        const ImageBuffer unmasked = develop(source, withoutMasks(state), request);
        const ImageBuffer actual =
            developOnGpu(context, source, state, Stage::Effects, request).readBack();
        REQUIRE(actual.size() == expected.size());
        const double colour = worstColourError(expected, actual);
        if (printsMeasured()) {
            std::fprintf(stderr, "brush end to end: %.3g\n", colour);
        }
        CAPTURE(colour, compareFloat(expected, actual, pointwiseAbsoluteFloor));
        REQUIRE(worstColourError(unmasked, expected) > 1e-3);
        REQUIRE(colour <= brushRelativeTolerance + resampleTolerance);
    };
    for (std::size_t level = 0; level < levels.size(); ++level) {
        INFO("level " << level << ", " << levels[level].size().width << "x"
                      << levels[level].size().height);
        requireRender(levels[level], {});
    }
    SECTION("fit inside") {
        requireRender(full, {.size = RenderRequest::FitInside{80, 80}});
    }
    SECTION("a region") {
        requireRender(full, {.size = RenderRequest::FitInside{60, 60},
                             .region = RenderRequest::Region{0.2, 0.1, 0.7, 0.9}});
    }
    SECTION("a halved source, rendered smaller") {
        requireRender(levels[1], {.size = RenderRequest::FitInside{50, 50}});
    }
}

TEST_CASE("The curve-input sample under a brush matches the CPU", "[gpu][brush]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = sceneOf({131, 77});
    const DevelopState state =
        stateOf({adjustmentOf(middleBrush(true), {{"exposure", 1.0}, {"clarity", 40.0}}),
                 adjustmentOf(someRadial(), {{"relativeTint", 70.0}, {"vibrance", 50.0}})});
    const ImageBuffer expected = sample(source, state, Tap::CurveInput);
    const ImageBuffer actual = sampleOnGpu(context, source, state, Tap::CurveInput);
    REQUIRE(actual.size() == expected.size());
    REQUIRE(actual.format() == expected.format());
    const double error = worstColourError(toRgbaF32(expected), toRgbaF32(actual));
    CAPTURE(error);
    REQUIRE(error <= 2.0 * brushRelativeTolerance + 1e-3);
    // The brush did something to the sample.
    const ImageBuffer plain = sample(source, withoutMasks(state), Tap::CurveInput);
    REQUIRE(worstColourError(toRgbaF32(plain), toRgbaF32(expected)) > 1e-3);
}

TEST_CASE("A brush on the camera-native fixture matches the CPU", "[gpu][brush]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = fixtureImage("bayer-32x24.dng");
    const DevelopState state = stateOf({adjustmentOf(
        middleBrush(true), {{"exposure", 1.0}, {"relativeTemperature", 40.0}}, 1.0F)});
    const ImageBuffer expected = develop(source, state);
    const ImageBuffer actual = developOnGpu(context, source, state).readBack();
    REQUIRE(actual.size() == expected.size());
    CAPTURE(compareFloat(expected, actual, pointwiseAbsoluteFloor));
    REQUIRE(worstColourError(toRgbaF32(expected), actual) <=
            brushRelativeTolerance + resampleTolerance);
}

TEST_CASE("The brush digest states match the CPU, through every stop and request", "[gpu][brush]") {
    GpuContext& context = gpuContext();
    const auto sources = digestSources();
    const auto requests = digestRequests();
    std::size_t compared = 0;
    for (const DigestState& named : digestMaskStates()) {
        if (named.name.rfind("mask-brush", 0) != 0) {
            continue;
        }
        for (const DigestSource& source : sources) {
            for (const DigestRequest& request : requests) {
                for (const Stage stop : digestStages) {
                    INFO(named.name << " / " << source.name << " / " << request.name << " / "
                                    << digestStageName(stop));
                    const ImageBuffer expected =
                        developUntil(source.buffer, named.state, stop, request.request).readBack();
                    const ImageBuffer actual =
                        developOnGpu(context, source.buffer, named.state, stop, request.request)
                            .readBack();
                    REQUIRE(actual.size() == expected.size());
                    REQUIRE(worstColourError(toRgbaF32(expected), toRgbaF32(actual)) <=
                            brushRelativeTolerance + resampleTolerance);
                    ++compared;
                }
                const ImageBuffer expected =
                    sample(source.buffer, named.state, Tap::CurveInput, request.request);
                const ImageBuffer actual = sampleOnGpu(context, source.buffer, named.state,
                                                       Tap::CurveInput, request.request);
                REQUIRE(actual.size() == expected.size());
                REQUIRE(worstColourError(toRgbaF32(expected), toRgbaF32(actual)) <=
                        2.0 * brushRelativeTolerance + 1e-3);
                ++compared;
            }
        }
    }
    REQUIRE(compared > 0);
}

namespace {

/// @brief Renders a state on the device through a ladder and reads the result back.
ImageBuffer ladderRender(GpuContext& context, CheckpointLadder& ladder,
                         const std::shared_ptr<const ImageBuffer>& source,
                         const DeviceImage& uploaded, const DevelopState& state,
                         const RenderRequest& request = {}, ProgressChannel* progress = nullptr) {
    return resumeOrDevelopOnGpu(context, ladder, source, uploaded, state, request, progress)
        .checkpoint.readBack();
}

/// @brief Renders a state on the device with no ladder and reads the result back.
ImageBuffer directRender(GpuContext& context, const ImageBuffer& source,
                         const DeviceImage& uploaded, const DevelopState& state,
                         const RenderRequest& request = {}) {
    return developOnGpu(context, source, uploaded, state, Stage::Effects, request).readBack();
}

/// @brief A source of 400 x 300 pixels: 4 x 3 tiles of 128.
std::shared_ptr<const ImageBuffer> tiledSource() {
    return std::make_shared<const ImageBuffer>(gentleOf({400, 300}));
}

/// @brief A state with two painted brushes on the plain settings.
DevelopState twoBrushes() {
    return stateOf({adjustmentOf(BrushMask{paintedMask(31, 4, everydayStyle, 0.75)},
                                 {{"exposure", 1.0}, {"shadows", 30.0}}),
                    adjustmentOf(BrushMask{paintedMask(32, 4, detailStyle, 0.75)},
                                 {{"saturation", 40.0}, {"texture", 30.0}})},
                   plainSettings());
}

/// @brief A small stroke in the top-left corner, touching few tiles.
Stroke cornerStroke() {
    return straightStroke({0.02F, 0.02F}, {0.08F, 0.06F}, 5, 0.02F, 0.6F, 1.0F);
}

} // namespace

TEST_CASE("A GPU ladder render with brushes equals a direct GPU render, through every kind of edit",
          "[gpu][brush][ladder]") {
    brushCoverageCache().clear();
    GpuContext& context = gpuContext();
    const auto source = tiledSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    const DevelopState first = twoBrushes();
    const LocalAdjustmentId firstId = first.localAdjustments[0].id;

    CheckpointLadder ladder;
    REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, first),
                     directRender(context, *source, uploaded, first)));
    CoverageResidency* residency = &LadderAccess::coverage(ladder);
    REQUIRE(residency->device() != nullptr);
    REQUIRE(residency->device()->planesUploaded() == 1);
    REQUIRE(residency->device()->rectanglesUploaded() == 0);
    REQUIRE_FALSE(residency->hasPending(0));

    SECTION("a delta edit does no coverage work and uploads nothing") {
        const std::uint64_t calls = residency->cacheCalls();
        const DevelopState louder = withLocalDelta(first, firstId, "exposure", 1.6);
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, louder),
                         directRender(context, *source, uploaded, louder)));
        REQUIRE(residency->cacheCalls() == calls);
        REQUIRE(residency->device()->planesUploaded() == 1);
        REQUIRE(residency->device()->rectanglesUploaded() == 0);
    }
    SECTION("an appended stroke uploads only the tiles it changed, and undo only those again") {
        const DevelopState appended = withStrokeAppended(first, firstId, cornerStroke());
        REQUIRE(appended != first);
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, appended),
                         directRender(context, *source, uploaded, appended)));
        const std::uint64_t afterAppend = residency->device()->rectanglesUploaded();
        REQUIRE(afterAppend > 0);
        // 4 x 3 tiles to a plane; a stroke in the corner touches a few.
        REQUIRE(afterAppend <= 4);
        REQUIRE(residency->device()->planesUploaded() == 1);

        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, first),
                         directRender(context, *source, uploaded, first)));
        const std::uint64_t afterUndo = residency->device()->rectanglesUploaded() - afterAppend;
        REQUIRE(afterUndo > 0);
        REQUIRE(afterUndo <= 4);
        REQUIRE(residency->device()->planesUploaded() == 1);
        REQUIRE_FALSE(residency->hasPending(0));
    }
    SECTION("disabling the first brush moves the second to its slot") {
        const DevelopState disabled = withLocalAdjustmentEnabled(first, firstId, false);
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, disabled),
                         directRender(context, *source, uploaded, disabled)));
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, first),
                         directRender(context, *source, uploaded, first)));
    }
    SECTION("a state without brushes lets the coverage go, and a brush brings it back") {
        const DevelopState plain = withoutMasks(first);
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, plain),
                         directRender(context, *source, uploaded, plain)));
        REQUIRE(LadderAccess::coverage(std::as_const(ladder)) == nullptr);
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, first),
                         directRender(context, *source, uploaded, first)));
        REQUIRE(LadderAccess::coverage(std::as_const(ladder)) != nullptr);
    }
    SECTION("a fifth brush makes a second texture") {
        DevelopState many = first;
        for (std::uint64_t seed = 40; seed < 43; ++seed) {
            many = withLocalAdjustmentAdded(
                std::move(many),
                adjustmentOf(BrushMask{paintedMask(seed, 2, everydayStyle, 0.75)},
                             {{"exposure", 0.3 + 0.2 * static_cast<double>(seed - 40)}}));
        }
        REQUIRE(planFor(*source, many).pointwise.local.brushCount() == 5);
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, many),
                         directRender(context, *source, uploaded, many)));
        REQUIRE(LadderAccess::coverage(ladder).device()->planesUploaded() == 2);
        // And back to four: the second texture goes.
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, first),
                         directRender(context, *source, uploaded, first)));
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, many),
                         directRender(context, *source, uploaded, many)));
    }
    SECTION("a coarser level of the same photograph starts the coverage again") {
        const auto small = std::make_shared<const ImageBuffer>(halved(*source));
        const DeviceImage smallUploaded = uploadSource(context, *small);
        REQUIRE(sameBits(ladderRender(context, ladder, small, smallUploaded, first),
                         directRender(context, *small, smallUploaded, first)));
    }
}

TEST_CASE("A stroke edit rendered on the CPU through a ladder, then on the GPU through the same "
          "one, equals a direct GPU render",
          "[gpu][brush][ladder]") {
    brushCoverageCache().clear();
    GpuContext& context = gpuContext();
    const auto source = tiledSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    const DevelopState first = twoBrushes();
    const DevelopState appended =
        withStrokeAppended(first, first.localAdjustments[1].id, cornerStroke());

    CheckpointLadder ladder;
    REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, first),
                     directRender(context, *source, uploaded, first)));
    // The CPU renders the edit: the planes move on, the textures do not.
    const LadderRender onHost = resumeOrDevelop(ladder, source, appended, {});
    REQUIRE(sameBits(onHost.checkpoint.readBack(), develop(*source, appended)));
    CoverageResidency& residency = LadderAccess::coverage(ladder);
    REQUIRE(residency.hasPending(0));
    const std::uint64_t rectangles = residency.device()->rectanglesUploaded();
    // The GPU renders it: the pending tiles go up, and nothing else.
    REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, appended),
                     directRender(context, *source, uploaded, appended)));
    REQUIRE_FALSE(residency.hasPending(0));
    REQUIRE(residency.device()->rectanglesUploaded() > rectangles);
    REQUIRE(residency.device()->rectanglesUploaded() - rectangles <= 4);
    REQUIRE(residency.device()->planesUploaded() == 1);
}

TEST_CASE("A GPU render cancelled while its coverage is made leaves the next one right",
          "[gpu][brush][ladder][cancel]") {
    brushCoverageCache().clear();
    GpuContext& context = gpuContext();
    const auto source = tiledSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    const DevelopState first = twoBrushes();
    const DevelopState appended =
        withStrokeAppended(first, first.localAdjustments[0].id, cornerStroke());
    const ImageBuffer reference = directRender(context, *source, uploaded, appended);

    std::size_t cancellations = 0;
    for (int cancelAt = 1; cancelAt <= 12; ++cancelAt) {
        INFO("cancelled at coverage report " << cancelAt);
        CheckpointLadder ladder;
        // The textures exist and hold the first state.
        static_cast<void>(ladderRender(context, ladder, source, uploaded, first));
        int seen = 0;
        ProgressChannel channel([&](const Progress& progress) {
            if (progress.step == ProgressStep::Coverage && ++seen >= cancelAt) {
                channel.cancel();
            }
        });
        bool cancelled = false;
        try {
            static_cast<void>(
                ladderRender(context, ladder, source, uploaded, appended, {}, &channel));
        } catch (const Cancelled&) {
            cancelled = true;
            ++cancellations;
        }
        // The next render, uncancelled, is right, and leaves nothing owed to the device.
        REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, appended), reference));
        REQUIRE_FALSE(LadderAccess::coverage(ladder).hasPending(0));
        if (!cancelled) {
            break;
        }
    }
    REQUIRE(cancellations > 0);
}

TEST_CASE("A residency ahead of its textures is brought up by the next GPU render",
          "[gpu][brush][ladder][cancel]") {
    // What a render cancelled between the residency's update and the upload leaves: the planes
    // are current, the textures are not, and the changed tiles are pending.
    brushCoverageCache().clear();
    GpuContext& context = gpuContext();
    const auto source = tiledSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    const DevelopState first = twoBrushes();
    const DevelopState appended =
        withStrokeAppended(first, first.localAdjustments[0].id, cornerStroke());

    CheckpointLadder ladder;
    static_cast<void>(ladderRender(context, ladder, source, uploaded, first));
    CoverageResidency& residency = LadderAccess::coverage(ladder);
    static_cast<void>(residency.update(planFor(*source, appended).pointwise.local, source->size()));
    REQUIRE(residency.hasPending(0));
    REQUIRE(sameBits(ladderRender(context, ladder, source, uploaded, appended),
                     directRender(context, *source, uploaded, appended)));
    REQUIRE_FALSE(residency.hasPending(0));
    REQUIRE(residency.device()->planesUploaded() == 1);
    REQUIRE(residency.device()->rectanglesUploaded() > 0);
    REQUIRE(residency.device()->rectanglesUploaded() <= 4);
}

TEST_CASE("A ladder moved to another device uploads its coverage whole again",
          "[gpu][brush][ladder]") {
    brushCoverageCache().clear();
    GpuContext& context = gpuContext();
    const auto source = tiledSource();
    const DeviceImage uploaded = uploadSource(context, *source);
    const DevelopState state = twoBrushes();
    CheckpointLadder ladder;
    static_cast<void>(ladderRender(context, ladder, source, uploaded, state));

    GpuContext other(gpuTestBackend());
    const DeviceImage otherUploaded = uploadSource(other, *source);
    REQUIRE(sameBits(ladderRender(other, ladder, source, otherUploaded, state),
                     directRender(other, *source, otherUploaded, state)));
    const DeviceCoverage* device = LadderAccess::coverage(std::as_const(ladder))->device();
    REQUIRE(device != nullptr);
    REQUIRE(device->device() == other.id());
    REQUIRE(device->planesUploaded() == 1);
}

TEST_CASE("The same path at different event rates is the same render on the device",
          "[gpu][brush][rates]") {
    GpuContext& context = gpuContext();
    // Dyadic corners, a power-of-two radius and even sides: every division in the dab placement
    // is exact, so splitting a segment at its midpoint changes no bit.
    const std::vector<float> corners{0.25F, 0.5F, 0.75F};
    const auto horizontal = [](const std::vector<float>& us) {
        Stroke stroke{0.0625F, 0.5F, 1.0F, false, {}};
        for (const float u : us) {
            stroke.points.push_back({u, 0.5F});
        }
        return stroke;
    };
    const auto split = [](std::vector<float> us, int times) {
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
    };
    for (const ImageSize size : {ImageSize{256, 192}, ImageSize{128, 96}}) {
        INFO(size.width << "x" << size.height);
        const auto source = std::make_shared<const ImageBuffer>(sceneOf(size));
        const DeviceImage uploaded = uploadSource(context, *source);
        const auto render = [&](int splits) {
            const DevelopState state =
                stateOf({adjustmentOf(brushOf({horizontal(split(corners, splits))}),
                                      {{"exposure", 1.0}, {"contrast", 20.0}})},
                        plainSettings());
            return directRender(context, *source, uploaded, state);
        };
        const ImageBuffer once = render(0);
        for (const int splits : {1, 2, 3}) {
            INFO(splits << " splits");
            requireBitIdentical(once, render(splits));
        }
        // Through a ladder, at level 1 (an exact half, both sides being even).
        const auto level1 = std::make_shared<const ImageBuffer>(halved(*source));
        const DeviceImage level1Uploaded = uploadSource(context, *level1);
        std::optional<ImageBuffer> reference;
        for (const int splits : {0, 1, 2, 3}) {
            const DevelopState state =
                stateOf({adjustmentOf(brushOf({horizontal(split(corners, splits))}),
                                      {{"exposure", 1.0}, {"contrast", 20.0}})},
                        plainSettings());
            CheckpointLadder ladder;
            ImageBuffer rendered = ladderRender(context, ladder, level1, level1Uploaded, state);
            if (!reference) {
                reference = std::move(rendered);
            } else {
                requireBitIdentical(*reference, rendered);
            }
        }
    }
}

TEST_CASE("A diagonal path at 1x and 3x renders alike except where a code differs, on the device",
          "[gpu][brush][rates]") {
    GpuContext& context = gpuContext();
    const ImageSize size{320, 240};
    const ImageBuffer source = sceneOf(size);
    const DeviceImage uploaded = uploadSource(context, source);
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
    const auto stateWith = [&](int perSegment) {
        return stateOf({adjustmentOf(brushOf({path(perSegment)}), {{"exposure", 1.0}})},
                       plainSettings());
    };
    const DevelopState one = stateWith(1);
    const DevelopState three = stateWith(3);
    const PackedCoverage a = packCoverage(planFor(source, one).pointwise.local);
    const PackedCoverage b = packCoverage(planFor(source, three).pointwise.local);
    const ImageBuffer renderOne = directRender(context, source, uploaded, one);
    const ImageBuffer renderThree = directRender(context, source, uploaded, three);
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
            if (ca != cb) {
                ++differing;
            } else {
                REQUIRE(pixelOf(renderOne, x, y) == pixelOf(renderThree, x, y));
            }
        }
    }
    REQUIRE(reached > 3000);
    REQUIRE(differing * 100 <= reached);
}

TEST_CASE("A hardness-one stroke is the global render at g + k inside, and nothing outside, at "
          "every level on the device",
          "[gpu][brush][levels]") {
    GpuContext& context = gpuContext();
    // The levels of an even source, a half-size decode of an odd one, and a tiny level.
    const std::vector<ImageSize> sizes{{256, 192}, {128, 96}, {64, 48}, {127, 95}, {33, 25}};
    for (const ImageSize size : sizes) {
        INFO(size.width << "x" << size.height);
        const ImageBuffer source = sceneOf(size);
        const DeviceImage uploaded = uploadSource(context, source);
        DevelopSettings settings = plainSettings();
        settings.tone.exposure = 0.25F;
        const DevelopState none{settings};
        DevelopSettings summed = plainSettings();
        summed.tone.exposure = 1.25F;
        // A fat stroke through the middle, a long-edge radius of a fifth.
        const DevelopState brushed =
            stateOf({adjustmentOf(
                        brushOf({straightStroke({0.3F, 0.5F}, {0.7F, 0.5F}, 12, 0.2F, 1.0F, 1.0F)}),
                        {{"exposure", 1.0}})},
                    settings);
        const ImageBuffer rendered = directRender(context, source, uploaded, brushed);
        const ImageBuffer unmasked = directRender(context, source, uploaded, none);
        const ImageBuffer allIn = directRender(context, source, uploaded, DevelopState{summed});
        const PackedCoverage packed = packCoverage(planFor(source, brushed).pointwise.local);
        std::size_t deep = 0;
        std::size_t outside = 0;
        double worst = 0.0;
        for (std::uint32_t y = 0; y < size.height; ++y) {
            for (std::uint32_t x = 0; x < size.width; ++x) {
                const std::uint8_t code = packed.code(0, x, y);
                if (code == 255) {
                    const auto got = pixelOf(rendered, x, y);
                    const auto want = pixelOf(allIn, x, y);
                    for (std::size_t c = 0; c < 3; ++c) {
                        worst = std::max(worst,
                                         static_cast<double>(std::abs(got[c] - want[c])) /
                                             std::max(1.0, std::abs(static_cast<double>(want[c]))));
                    }
                    ++deep;
                } else if (code == 0) {
                    REQUIRE(pixelOf(rendered, x, y) == pixelOf(unmasked, x, y));
                    ++outside;
                }
            }
        }
        if (printsMeasured()) {
            std::fprintf(stderr, "brush full weight: worst %.3g\n", worst);
        }
        REQUIRE(worst <= brushRelativeTolerance);
        REQUIRE(deep > 0);
        REQUIRE(outside > 0);
    }
}

TEST_CASE("The coarser first render's sequence on the device: level 1 direct, then level 0 "
          "through the ladder",
          "[gpu][brush][ladder][provisional]") {
    brushCoverageCache().clear();
    GpuContext& context = gpuContext();
    const auto source = std::make_shared<const ImageBuffer>(gentleOf({512, 384}));
    const auto level1 = std::make_shared<const ImageBuffer>(halved(*source));
    const DeviceImage uploaded = uploadSource(context, *source);
    const DeviceImage level1Uploaded = uploadSource(context, *level1);
    const DevelopState state = twoBrushes();
    const RenderRequest request{.size = RenderRequest::FitInside{200, 200}};

    CheckpointLadder ladder;
    // Nothing is made: the level-0 render would draw from nothing, whichever backend renders.
    REQUIRE(drawsBrushCoverageOnGpu(context, ladder, *source, state, request));
    // The stand-in: a direct render at level 1, no ladder touched.
    const ImageBuffer standIn =
        developOnGpu(context, *level1, level1Uploaded, state, Stage::Effects, request).readBack();
    REQUIRE(ladder.empty());
    REQUIRE(LadderAccess::coverage(std::as_const(ladder)) == nullptr);
    // It made level-1 coverage in the cache, not level 0's: still from nothing at level 0.
    REQUIRE(drawsBrushCoverageOnGpu(context, ladder, *source, state, request));

    // Then level 0, through the ladder: the same bits as a direct GPU render.
    const ImageBuffer shown = ladderRender(context, ladder, source, uploaded, state, request);
    REQUIRE(sameBits(shown, directRender(context, *source, uploaded, state, request)));
    REQUIRE(standIn.size() == shown.size());

    // Warm: the cache, the residency and the GPU's rungs are all there.
    REQUIRE_FALSE(drawsBrushCoverageOnGpu(context, ladder, *source, state, request));
    // A delta edit invalidates the pointwise rung, and the residency holds the coverage.
    const DevelopState louder =
        withLocalDelta(state, state.localAdjustments[0].id, "exposure", 1.5);
    REQUIRE_FALSE(drawsBrushCoverageOnGpu(context, ladder, *source, louder, request));
    // An appended stroke extends the cached list: not from nothing either.
    const DevelopState appended =
        withStrokeAppended(state, state.localAdjustments[0].id, cornerStroke());
    REQUIRE_FALSE(drawsBrushCoverageOnGpu(context, ladder, *source, appended, request));

    SECTION("a rung alone answers, for the device that holds it") {
        // A copy keeps the rungs and drops the residency; with the cache cleared, only a rung
        // can say that nothing is drawn.
        const CheckpointLadder copy = ladder;
        brushCoverageCache().clear();
        REQUIRE_FALSE(drawsBrushCoverageOnGpu(context, copy, *source, state, request));
        // The host's question does not count a device rung.
        REQUIRE(drawsBrushCoverage(copy, *source, state, request));
    }
    SECTION("the cache alone answers") {
        // Without the rungs and the residency, the cache still holds level 0's coverage.
        ladder.clear();
        REQUIRE_FALSE(drawsBrushCoverageOnGpu(context, ladder, *source, state, request));
    }
}

TEST_CASE("The pointwise pass reads eleven images", "[gpu][brush]") {
    GpuContext& context = gpuContext();
    const DeviceImage image = context.upload(sceneOf({8, 8}));
    const GpuPointwiseBlock block =
        packPointwise(planFor(sceneOf({8, 8}), DevelopState{}).pointwise, image.size());
    const std::span<const std::byte> bytes = std::as_bytes(std::span(&block, 1));
    // Seven, the count before the coverage textures, is refused; eleven is a render.
    const std::vector<DeviceImage> seven(7, image);
    REQUIRE_THROWS_AS(
        context.render(GpuPass::Pointwise, bytes, seven, image.size(), workingEncoding),
        std::invalid_argument);
    const std::vector<DeviceImage> eleven(11, image);
    const DeviceImage rendered =
        context.render(GpuPass::Pointwise, bytes, eleven, image.size(), workingEncoding);
    REQUIRE(rendered.size() == image.size());
}
