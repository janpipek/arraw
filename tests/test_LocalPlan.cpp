#include "LocalPlan.h"
#include "Presence.h"
#include "ProcessingPlan.h"
#include "support/FieldCount.h"
#include "support/LadderTesting.h"

#include <CheckpointLadder.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>
#include <LocalAdjustmentEdits.h>
#include <LocalAdjustments.h>
#include <Photo.h>
#include <SettingDescriptors.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace arraw;
using namespace arraw::test;

/// The local adjustments in the plan and in the CPU chain (ADR 044, sections 2 to 5 and 8).

namespace {

/// @brief A working-space image whose colour each pixel gets from a function.
ImageBuffer imageOf(ImageSize size, const std::function<Colour(std::uint32_t, std::uint32_t)>& at,
                    ImageOrientation orientation = ImageOrientation::Normal) {
    ImageBuffer image(size, workingFormat, workingEncoding, orientation);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const Colour colour = at(x, y);
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = colour[0];
            pixel[1] = colour[1];
            pixel[2] = colour[2];
            pixel[3] = 1.0F;
        }
    }
    return image;
}

/// @brief A flat grey image.
ImageBuffer flat(ImageSize size, float value = 0.2F) {
    return imageOf(size, [value](auto, auto) { return Colour{value, value, value}; });
}

/// @brief A smooth coloured scene of fine detail and broad shapes, different at every pixel.
Colour sceneColour(std::uint32_t x, std::uint32_t y) {
    const double u = x;
    const double v = y;
    const double shapes =
        0.5 * std::sin(u / 37.0) * std::cos(v / 23.0) + 0.3 * std::sin((u + v) / 61.0);
    const double detail = 0.05 * std::sin(u / 2.3) * std::sin(v / 3.1);
    const auto base = static_cast<float>(0.18 * std::exp2(shapes + detail));
    return {base * 1.2F, base, base * 0.7F};
}

ImageBuffer sceneOf(ImageSize size, ImageOrientation orientation = ImageOrientation::Normal) {
    return imageOf(size, sceneColour, orientation);
}

/// @brief Settings with no highlight roll-off, so that nothing bends a bright result.
DevelopSettings plainSettings() {
    DevelopSettings settings;
    settings.tone.filmicHighlights = 0.0F;
    return settings;
}

/// @brief Appends an enabled adjustment.
DevelopState withMask(DevelopState state, Mask shape, const LocalDeltas& deltas,
                      float opacity = 1.0F, bool invert = false) {
    LocalAdjustment adjustment;
    adjustment.shape = std::move(shape);
    adjustment.deltas = deltas;
    adjustment.opacity = opacity;
    adjustment.invert = invert;
    return withLocalAdjustmentAdded(std::move(state), std::move(adjustment));
}

/// @brief A radial mask whose weight is exactly one on any frame the tests use.
RadialMask everywhere() {
    return {.centre = {0.5F, 0.5F},
            .radiusX = maximumMaskRadius,
            .radiusY = maximumMaskRadius,
            .angle = 0.0F,
            .feather = 0.0F};
}

/// @brief Sets one global control by its key.
void setGlobal(DevelopSettings& settings, std::string_view key, float value) {
    if (key == "exposure") {
        settings.tone.exposure = value;
    } else if (key == "contrast") {
        settings.tone.contrast = value;
    } else if (key == "highlights") {
        settings.tone.highlights = value;
    } else if (key == "shadows") {
        settings.tone.shadows = value;
    } else if (key == "whites") {
        settings.tone.whites = value;
    } else if (key == "blacks") {
        settings.tone.blacks = value;
    } else if (key == "texture") {
        settings.presence.texture = value;
    } else if (key == "clarity") {
        settings.presence.clarity = value;
    } else if (key == "dehaze") {
        settings.presence.dehaze = value;
    } else if (key == "saturation") {
        settings.color.saturation = value;
    } else if (key == "vibrance") {
        settings.color.vibrance = value;
    } else {
        FAIL("no global control " << key);
    }
}

/// @brief The weight a state's first resolved mask has at a pixel.
float weightAt(const DevelopState& state, ImageSize size, std::uint32_t x, std::uint32_t y,
               std::size_t index = 0) {
    const LocalPlan plan = localPlanFor(state, size);
    return maskWeight(plan.masks.at(index), static_cast<float>(x) + 0.5F,
                      static_cast<float>(y) + 0.5F);
}

double smoothstepRef(double first, double last, double value) {
    const double t = std::clamp((value - first) / (last - first), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// @brief The weight of a linear mask worked out from the ADR's words, in double.
double linearRef(const LinearMask& mask, ImageSize size, std::uint32_t x, std::uint32_t y) {
    const double longEdge = std::max(size.width, size.height);
    const double px = (x + 0.5) / longEdge;
    const double py = (y + 0.5) / longEdge;
    const double ax = mask.from.u * size.width / longEdge;
    const double ay = mask.from.v * size.height / longEdge;
    const double dx = mask.to.u * size.width / longEdge - ax;
    const double dy = mask.to.v * size.height / longEdge - ay;
    const double t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    return 1.0 - smoothstepRef(0.0, 1.0, t);
}

/// @brief The weight of a radial mask worked out from the ADR's words, in double.
double radialRef(const RadialMask& mask, ImageSize size, std::uint32_t x, std::uint32_t y) {
    const double longEdge = std::max(size.width, size.height);
    const double dx = (x + 0.5) / longEdge - mask.centre.u * size.width / longEdge;
    const double dy = (y + 0.5) / longEdge - mask.centre.v * size.height / longEdge;
    const double angle = mask.angle * std::numbers::pi / 180.0;
    const double qx = dx * std::cos(angle) + dy * std::sin(angle);
    const double qy = -dx * std::sin(angle) + dy * std::cos(angle);
    const double d = std::hypot(qx / mask.radiusX, qy / mask.radiusY);
    const double pixel = 1.0 / longEdge / std::min(mask.radiusX, mask.radiusY);
    const double inner = 1.0 - std::max<double>(mask.feather, pixel);
    return 1.0 - smoothstepRef(inner, 1.0, d);
}

/// @brief Whether two colours hold the same bits.
bool sameColour(const Colour& a, const Colour& b) {
    return std::memcmp(a.data(), b.data(), sizeof(Colour)) == 0;
}

/// @brief The pixel of a working-format image.
Colour pixelOf(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto samples = image.samples<float>();
    const float* pixel = &samples[(static_cast<std::size_t>(y) * image.size().width + x) * 4];
    return {pixel[0], pixel[1], pixel[2]};
}

/// @brief Largest channel difference between two images of one size.
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

/// @brief The plan of a state against a source's size, as the render uses it.
ProcessingPlan planOf(const ImageBuffer& source, const DevelopState& state) {
    return planFor(source, state);
}

/// @brief Luminance of a colour in the working space.
float luminanceOf(const Colour& colour) {
    return colorspaces::workingLuminance[0] * colour[0] +
           colorspaces::workingLuminance[1] * colour[1] +
           colorspaces::workingLuminance[2] * colour[2];
}

} // namespace

TEST_CASE("The local block is the default when no mask does anything", "[local][plan]") {
    const ImageBuffer source = flat({16, 16});
    const DevelopState none{plainSettings()};
    REQUIRE(planOf(source, none).pointwise.local == LocalPlan{});

    const LocalDeltas some{.exposure = 1.0F, .dehaze = 20.0F};
    SECTION("a disabled mask") {
        DevelopState state = withMask(none, everywhere(), some);
        state = withLocalAdjustmentEnabled(state, state.localAdjustments[0].id, false);
        REQUIRE(planOf(source, state).pointwise.local == LocalPlan{});
        REQUIRE(planOf(source, state).pointwise == planOf(source, none).pointwise);
    }
    SECTION("a mask at opacity zero") {
        const DevelopState state = withMask(none, everywhere(), some, 0.0F);
        REQUIRE(planOf(source, state).pointwise == planOf(source, none).pointwise);
    }
    SECTION("a mask whose deltas are all zero") {
        const DevelopState state = withMask(none, LinearMask{}, LocalDeltas{});
        REQUIRE(planOf(source, state).pointwise == planOf(source, none).pointwise);
    }
    SECTION("the overloads that know no size") {
        const DevelopState state = withMask(none, everywhere(), some);
        REQUIRE(planFor(workingEncoding, state).pointwise.local == LocalPlan{});
    }
}

TEST_CASE("The local block holds the masks that act, with k, the touched bits and the globals",
          "[local][plan]") {
    DevelopSettings settings = plainSettings();
    settings.tone.exposure = 0.5F;
    settings.tone.highlights = -20.0F;
    settings.presence.clarity = 30.0F;
    settings.color.vibrance = 12.0F;
    DevelopState state{settings};
    state = withMask(state, LinearMask{}, {.exposure = 1.0F, .saturation = 10.0F}, 0.5F);
    state = withMask(state, RadialMask{}, {.dehaze = -40.0F});
    // Disabled, so absent, and not counted in the order.
    state = withMask(state, RadialMask{}, {.contrast = 5.0F});
    state = withLocalAdjustmentEnabled(state, state.localAdjustments[2].id, false);
    state = withMask(state, LinearMask{}, {.relativeTint = 30.0F});

    const LocalPlan local = planOf(flat({40, 20}), state).pointwise.local;
    REQUIRE(local.masks.size() == 3);
    REQUIRE(local.masks[0].kind == LocalMaskKind::Linear);
    REQUIRE(local.masks[1].kind == LocalMaskKind::Radial);
    REQUIRE(local.masks[2].kind == LocalMaskKind::Linear);
    // Opacity is folded into k, in the table's order.
    REQUIRE(local.masks[0].k[indexOf(LocalControl::Exposure)] == 0.5F);
    REQUIRE(local.masks[0].k[indexOf(LocalControl::Saturation)] == 5.0F);
    REQUIRE(local.masks[1].k[indexOf(LocalControl::Dehaze)] == -40.0F);
    REQUIRE(local.masks[2].k[indexOf(LocalControl::RelativeTint)] == 30.0F);
    REQUIRE(local.touched == (bitOf(LocalControl::Exposure) | bitOf(LocalControl::Saturation) |
                              bitOf(LocalControl::Dehaze) | bitOf(LocalControl::RelativeTint)));
    REQUIRE(local.global[indexOf(LocalControl::Exposure)] == 0.5F);
    REQUIRE(local.global[indexOf(LocalControl::Highlights)] == -20.0F);
    REQUIRE(local.global[indexOf(LocalControl::Clarity)] == 30.0F);
    REQUIRE(local.global[indexOf(LocalControl::Vibrance)] == 12.0F);
    REQUIRE(local.global[indexOf(LocalControl::RelativeTemperature)] == 0.0F);
}

TEST_CASE("A plan from a photograph resolves the masks as one from its pixels does",
          "[local][plan]") {
    DevelopSettings settings = plainSettings();
    settings.presence.dehaze = -30.0F;
    DevelopState state{settings};
    state = withMask(state, LinearMask{}, {.exposure = 1.0F, .dehaze = 60.0F});
    state = withMask(state, RadialMask{}, {.texture = 20.0F});
    const Photo photo("photo.dng", ImageMetadata{ImageSize{40, 20}, workingEncoding}, state);
    const ProcessingPlan fromPhoto = planFor(photo);
    const ProcessingPlan fromPixels = planOf(flat({40, 20}), state);
    REQUIRE_FALSE(fromPhoto.pointwise.local.empty());
    REQUIRE(fromPhoto.pointwise.local == fromPixels.pointwise.local);
    REQUIRE(fromPhoto.pointwise.presence == fromPixels.pointwise.presence);
    REQUIRE(fromPhoto.pointwise.presence.hazeFloor.active());
    REQUIRE(fromPhoto.pointwise.presence.hazeMean.active());
    REQUIRE(fromPhoto.pointwise.presence.fine.active());
}

TEST_CASE("A plan built twice from equal states compares equal", "[local][plan]") {
    const ImageBuffer source = flat({40, 20});
    DevelopState state{plainSettings()};
    state = withMask(state, LinearMask{}, {.exposure = 1.0F});
    state = withMask(state, RadialMask{}, {.texture = 40.0F}, 0.7F, true);
    const DevelopState copy = state;
    REQUIRE(planOf(source, state) == planOf(source, copy));
    REQUIRE(planOf(source, state).pointwise.local == planOf(source, copy).pointwise.local);
    // The same state against another size is another plan.
    REQUIRE_FALSE(planOf(source, state).pointwise.local ==
                  planOf(flat({41, 20}), state).pointwise.local);
}

TEST_CASE("The plan's blocks have no stray field", "[local][plan]") {
    STATIC_REQUIRE(test::fieldCount<LocalPlan> == 3);
    STATIC_REQUIRE(test::fieldCount<LocalMaskPlan> == 10);
    STATIC_REQUIRE(test::fieldCount<PreTapMask> == 10);
    STATIC_REQUIRE(test::fieldCount<PreTapLocal> == 2);
    STATIC_REQUIRE(test::fieldCount<PixelAmounts> == 5);
    STATIC_REQUIRE(test::fieldCount<PresencePlan> == 6);
}

TEST_CASE("A linear mask is one at its start, zero at its end and smooth between",
          "[local][weight]") {
    const ImageSize size{100, 100};
    const DevelopState state =
        withMask(DevelopState{}, LinearMask{{0.5F, 0.25F}, {0.5F, 0.75F}}, {.exposure = 1.0F});
    // Pixel centres at 25.5 and 74.5 of 100: a half pixel either side of the ends.
    REQUIRE(weightAt(state, size, 50, 24) > 0.99F);
    REQUIRE(weightAt(state, size, 50, 25) > 0.99F);
    REQUIRE(weightAt(state, size, 50, 74) < 0.01F);
    REQUIRE(weightAt(state, size, 50, 5) == 1.0F);
    REQUIRE(weightAt(state, size, 50, 95) == 0.0F);
    // Half way: the middle of the ramp, between rows 49 and 50.
    REQUIRE(weightAt(state, size, 50, 49) + weightAt(state, size, 50, 50) ==
            Catch::Approx(1.0F).margin(1e-6));
    // Constant along a line perpendicular to the direction, and falling along it.
    for (std::uint32_t y = 25; y < 75; ++y) {
        REQUIRE(weightAt(state, size, 3, y) == weightAt(state, size, 90, y));
        REQUIRE(weightAt(state, size, 50, y + 1) <= weightAt(state, size, 50, y));
    }
}

TEST_CASE("A linear mask's bands are perpendicular to its line on a frame that is not square",
          "[local][weight]") {
    const ImageSize size{300, 100};
    // The line runs 45 degrees on screen: from (60, 20) to (140, 100) pixels... in normalised
    // coordinates, which is what the state holds.
    const LinearMask mask{{60.0F / 300.0F, 0.2F}, {140.0F / 300.0F, 1.0F}};
    const DevelopState state = withMask(DevelopState{}, mask, {.exposure = 1.0F});
    // A step along the line's perpendicular, (1, -1) in pixels, leaves the weight alone.
    for (const auto [x, y] : {std::pair{100U, 50U}, std::pair{90U, 30U}, std::pair{120U, 70U}}) {
        INFO(x << ", " << y);
        const float centre = weightAt(state, size, x, y);
        REQUIRE(centre > 0.05F);
        REQUIRE(centre < 0.95F);
        REQUIRE(weightAt(state, size, x + 5, y - 5) == Catch::Approx(centre).margin(2e-5));
        REQUIRE(weightAt(state, size, x - 6, y + 6) == Catch::Approx(centre).margin(2e-5));
    }
    // And a step along it changes the weight by the isotropic amount, not by the frame's aspect.
    for (std::uint32_t x = 60; x < 140; x += 7) {
        REQUIRE(weightAt(state, size, x, x - 40) ==
                Catch::Approx(linearRef(mask, size, x, x - 40)).margin(2e-5));
    }
}

TEST_CASE("A radial mask is one inside its inner oval, zero at its edge, and round on screen",
          "[local][weight]") {
    const ImageSize size{300, 100};
    RadialMask mask{
        .centre = {0.5F, 0.5F}, .radiusX = 0.2F, .radiusY = 0.2F, .angle = 0.0F, .feather = 0.5F};
    const DevelopState state = withMask(DevelopState{}, mask, {.exposure = 1.0F});
    // Radius 0.2 of the long edge: 60 pixels. The centre is (150, 50).
    REQUIRE(weightAt(state, size, 150, 50) == 1.0F);
    REQUIRE(weightAt(state, size, 150 + 25, 50) == 1.0F); // within the inner half
    REQUIRE(weightAt(state, size, 150 + 61, 50) == 0.0F);
    REQUIRE(weightAt(state, size, 150, 50 + 49) == 0.0F + weightAt(state, size, 150, 50 + 49));
    // The same distance across and down is the same weight: a circle on screen.
    for (const std::uint32_t distance : {10U, 35U, 45U, 55U}) {
        INFO(distance);
        const float across = weightAt(state, size, 150 + distance, 50);
        const float down = weightAt(state, size, 150, 50 + distance);
        REQUIRE(across == Catch::Approx(down).margin(2e-5));
    }
    // Half way across the feather the weight is a half.
    const float halfway = weightAt(state, size, 150 + 45, 50);
    REQUIRE(halfway == Catch::Approx(radialRef(mask, size, 150 + 45, 50)).margin(2e-5));
    REQUIRE(halfway > 0.1F);
    REQUIRE(halfway < 0.9F);
}

TEST_CASE("A radial mask's angle turns the x radius from +x towards +y", "[local][weight]") {
    const ImageSize size{200, 200};
    // Long along x, then turned a quarter: long along y (down on screen).
    RadialMask mask{
        .centre = {0.5F, 0.5F}, .radiusX = 0.4F, .radiusY = 0.1F, .angle = 0.0F, .feather = 0.2F};
    const DevelopState flatOval = withMask(DevelopState{}, mask, {.exposure = 1.0F});
    mask.angle = 90.0F;
    const DevelopState upright = withMask(DevelopState{}, mask, {.exposure = 1.0F});
    REQUIRE(weightAt(flatOval, size, 100 + 50, 100) == 1.0F);
    REQUIRE(weightAt(flatOval, size, 100, 100 + 50) == 0.0F);
    REQUIRE(weightAt(upright, size, 100, 100 + 50) == 1.0F);
    REQUIRE(weightAt(upright, size, 100 + 50, 100) == 0.0F);
    // Forty-five degrees: along the diagonal that runs down to the right.
    mask.angle = 45.0F;
    const DevelopState diagonal = withMask(DevelopState{}, mask, {.exposure = 1.0F});
    REQUIRE(weightAt(diagonal, size, 100 + 40, 100 + 40) == 1.0F);
    REQUIRE(weightAt(diagonal, size, 100 + 40, 100 - 40) == 0.0F);
}

TEST_CASE("A radial mask's feather: zero is a hard edge a pixel wide, and one is the whole radius",
          "[local][weight]") {
    const ImageSize size{200, 100};
    const auto inner = [&](float feather) {
        const RadialMask mask{.centre = {0.5F, 0.5F},
                              .radiusX = 0.25F,
                              .radiusY = 0.25F,
                              .angle = 0.0F,
                              .feather = feather};
        const DevelopState state = withMask(DevelopState{}, mask, {.exposure = 1.0F});
        return localPlanFor(state, size).masks[0].inner;
    };
    // One pixel of the long edge over the short radius: 1/200 / 0.25.
    const float pixel = 1.0F / 200.0F / 0.25F;
    REQUIRE(inner(0.0F) == Catch::Approx(1.0F - pixel).margin(1e-7));
    // A feather narrower than a pixel renders as the pixel.
    REQUIRE(inner(0.5F * pixel) == inner(0.0F));
    REQUIRE(inner(pixel) == inner(0.0F));
    REQUIRE(inner(0.5F) == Catch::Approx(0.5F));
    REQUIRE(inner(1.0F) == 0.0F);

    // A hard edge: full inside, gone a pixel and a bit outside.
    const RadialMask hard{
        .centre = {0.5F, 0.5F}, .radiusX = 0.25F, .radiusY = 0.25F, .angle = 0.0F, .feather = 0.0F};
    const DevelopState state = withMask(DevelopState{}, hard, {.exposure = 1.0F});
    REQUIRE(weightAt(state, size, 100 + 48, 50) == 1.0F);
    REQUIRE(weightAt(state, size, 100 + 50, 50) == 0.0F);
}

TEST_CASE("An inverted mask has the weight that is left over", "[local][weight]") {
    const ImageSize size{97, 61};
    const std::vector<Mask> shapes{LinearMask{{0.2F, 0.3F}, {0.7F, 0.9F}}, RadialMask{}};
    for (const Mask& shape : shapes) {
        const DevelopState plain = withMask(DevelopState{}, shape, {.exposure = 1.0F});
        const DevelopState inverted =
            withMask(DevelopState{}, shape, {.exposure = 1.0F}, 1.0F, true);
        for (std::uint32_t y = 0; y < size.height; y += 6) {
            for (std::uint32_t x = 0; x < size.width; x += 5) {
                REQUIRE(weightAt(plain, size, x, y) + weightAt(inverted, size, x, y) ==
                        Catch::Approx(1.0F).margin(1e-6));
            }
        }
    }
}

TEST_CASE("Masks outside the frame still weigh in it", "[local][weight]") {
    const ImageSize size{64, 48};
    // A linear mask whose whole ramp lies to the left of the frame: zero everywhere.
    const DevelopState away =
        withMask(DevelopState{}, LinearMask{{-1.5F, 0.5F}, {-0.5F, 0.5F}}, {.exposure = 1.0F});
    REQUIRE(weightAt(away, size, 0, 0) == 0.0F);
    REQUIRE(weightAt(away, size, 63, 47) == 0.0F);
    // A radial one centred off the frame reaches into it.
    const DevelopState near = withMask(
        DevelopState{}, RadialMask{{-0.05F, 0.5F}, 0.3F, 0.3F, 0.0F, 0.5F}, {.exposure = 1.0F});
    REQUIRE(weightAt(near, size, 0, 24) > 0.9F);
    REQUIRE(weightAt(near, size, 63, 24) == 0.0F);
}

TEST_CASE("A render's pixels follow the weight worked out from the ADR's words",
          "[local][render]") {
    const std::vector<ImageSize> sizes{{97, 61}, {64, 64}, {33, 100}, {5, 3}, {2, 2}, {200, 17}};
    for (const ImageSize size : sizes) {
        INFO(size.width << "x" << size.height);
        const ImageBuffer source = flat(size);
        const LinearMask linear{{0.15F, 0.3F}, {0.8F, 0.75F}};
        const RadialMask radial{.centre = {0.4F, 0.6F},
                                .radiusX = 0.35F,
                                .radiusY = 0.2F,
                                .angle = 25.0F,
                                .feather = 0.4F};
        const DevelopState state =
            withMask(withMask(DevelopState{plainSettings()}, linear, {.exposure = 2.0F}), radial,
                     {.exposure = -1.0F}, 1.0F, true);
        const ImageBuffer out = develop(source, state);
        for (std::uint32_t y = 0; y < size.height; ++y) {
            for (std::uint32_t x = 0; x < size.width; ++x) {
                const double stops = 2.0 * linearRef(linear, size, x, y) -
                                     1.0 * (1.0 - radialRef(radial, size, x, y));
                const double expected = 0.2 * std::exp2(stops);
                REQUIRE(pixelOf(out, x, y)[1] ==
                        Catch::Approx(expected).epsilon(3e-5).margin(1e-6));
            }
        }
    }
}

TEST_CASE("Masks sum in setting units and clamp once", "[local][sum]") {
    const ImageSize size{32, 32};
    const ImageBuffer source = sceneOf(size);
    const LinearMask ramp{{0.5F, 0.0F}, {0.5F, 1.0F}};

    SECTION("two equal masks are one of twice the amount") {
        const DevelopState two =
            withMask(withMask(DevelopState{plainSettings()}, ramp,
                              {.relativeTemperature = 50.0F, .exposure = 0.75F}),
                     ramp, {.relativeTemperature = 50.0F, .exposure = 0.75F});
        const DevelopState one = withMask(DevelopState{plainSettings()}, ramp,
                                          {.relativeTemperature = 100.0F, .exposure = 1.5F});
        REQUIRE(sameBits(develop(source, two), develop(source, one)));
    }
    SECTION("masks that cancel exactly leave the global render, bit for bit") {
        DevelopSettings settings = plainSettings();
        settings.tone.exposure = 0.4F;
        settings.tone.contrast = 15.0F;
        const DevelopState without{settings};
        const LocalDeltas up{.relativeTemperature = 30.0F,
                             .exposure = 1.0F,
                             .contrast = 40.0F,
                             .highlights = -20.0F,
                             .saturation = 25.0F,
                             .vibrance = 10.0F};
        LocalDeltas down;
        for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
            down.*descriptor.member = -(up.*descriptor.member);
        }
        const DevelopState cancelling = withMask(withMask(without, ramp, up), ramp, down);
        REQUIRE(planOf(source, cancelling).pointwise.local.masks.size() == 2);
        REQUIRE(sameBits(develop(source, cancelling), develop(source, without)));
    }
    SECTION("sixteen overlapping masks are one carrying the sum") {
        DevelopState sixteen{plainSettings()};
        for (std::size_t index = 0; index < maximumLocalAdjustments; ++index) {
            sixteen = withMask(sixteen, ramp, {.exposure = 0.125F, .shadows = 5.0F});
        }
        const DevelopState one =
            withMask(DevelopState{plainSettings()}, ramp, {.exposure = 2.0F, .shadows = 80.0F});
        // Sixteen sums of a power of two are exact.
        REQUIRE(sameBits(develop(source, sixteen), develop(source, one)));
    }
    SECTION("the sum clamps to the global range, once") {
        DevelopSettings settings = plainSettings();
        settings.tone.exposure = 4.5F;
        // Two full-weight masks of +1: 6.5 clamps to the range's 5.
        const DevelopState state =
            withMask(withMask(DevelopState{settings}, everywhere(), {.exposure = 1.0F}),
                     everywhere(), {.exposure = 1.0F});
        const PixelAmounts at = amountsAt(planOf(source, state).pointwise, 3, 4);
        REQUIRE(at.tone.exposureGain == exposureGainFor(brightestExposure));
        // Masks that overshoot and come back: the clamp is on the total, not on each step.
        settings.tone.exposure = 3.0F;
        const DevelopState opposed =
            withMask(withMask(DevelopState{settings}, everywhere(), {.exposure = 4.0F}),
                     everywhere(), {.exposure = -1.0F});
        REQUIRE(amountsAt(planOf(source, opposed).pointwise, 3, 4).tone.exposureGain ==
                exposureGainFor(brightestExposure));
    }
}

TEST_CASE("A full-weight mask is the global render at the summed setting", "[local][sum]") {
    const ImageSize size{40, 30};
    const ImageBuffer source = sceneOf(size);
    struct Row {
        const char* key;
        float global;
        float local;
    };
    // Each control alone, then past the clamp (the last of each pair).
    const std::vector<Row> rows{
        {"exposure", 0.5F, 1.25F},   {"exposure", 3.0F, 2.5F},      {"contrast", 20.0F, 35.0F},
        {"contrast", 80.0F, 50.0F},  {"highlights", -10.0F, 40.0F}, {"highlights", 60.0F, 70.0F},
        {"shadows", 30.0F, 25.0F},   {"shadows", -90.0F, -40.0F},   {"whites", 15.0F, 30.0F},
        {"whites", 0.0F, -100.0F},   {"blacks", -25.0F, 60.0F},     {"blacks", 90.0F, 40.0F},
        {"texture", 0.0F, 45.0F},    {"texture", 70.0F, 60.0F},     {"clarity", 20.0F, 30.0F},
        {"clarity", -50.0F, -80.0F}, {"dehaze", 0.0F, 40.0F},       {"dehaze", 50.0F, 30.0F},
        {"dehaze", 0.0F, -40.0F},    {"saturation", 10.0F, 35.0F},  {"saturation", -70.0F, -60.0F},
        {"vibrance", 0.0F, 55.0F},   {"vibrance", 80.0F, 60.0F}};
    for (const Row& row : rows) {
        INFO(row.key << " " << row.global << " + " << row.local);
        const float most = std::string_view(row.key) == "exposure" ? 5.0F : 100.0F;
        DevelopSettings globalSettings = plainSettings();
        setGlobal(globalSettings, row.key, row.global);
        DevelopSettings summedSettings = plainSettings();
        setGlobal(summedSettings, row.key, std::clamp(row.global + row.local, -most, most));
        const DevelopState globalOnly{globalSettings};
        const DevelopState summed{summedSettings};
        LocalDeltas deltas;
        deltas.*(findLocalDescriptor(row.key)->member) = row.local;
        const DevelopState masked = withMask(globalOnly, everywhere(), deltas);
        REQUIRE(largestDifference(develop(source, masked), develop(source, summed)) <= 2e-6F);
    }
}

TEST_CASE("Masks that do nothing leave the render bit for bit", "[local][neutral]") {
    const ImageBuffer source = sceneOf({48, 36});
    DevelopSettings settings = plainSettings();
    settings.tone.exposure = 0.3F;
    settings.presence.clarity = 25.0F;
    const DevelopState without{settings};
    const ImageBuffer reference = develop(source, without);
    const LocalDeltas some{
        .relativeTemperature = 20.0F, .exposure = 1.0F, .texture = 30.0F, .dehaze = 25.0F};

    SECTION("disabled, opacity zero, and no delta") {
        DevelopState state = withMask(without, LinearMask{}, some);
        state = withLocalAdjustmentEnabled(state, state.localAdjustments[0].id, false);
        state = withMask(state, RadialMask{}, some, 0.0F);
        state = withMask(state, RadialMask{}, LocalDeltas{});
        REQUIRE(sameBits(develop(source, state), reference));
    }
    SECTION("outside every mask's coverage") {
        const RadialMask corner{.centre = {0.1F, 0.1F},
                                .radiusX = 0.1F,
                                .radiusY = 0.1F,
                                .angle = 0.0F,
                                .feather = 0.5F};
        const DevelopState state = withMask(without, corner, some);
        const ImageBuffer masked = develop(source, state);
        std::size_t outside = 0;
        std::size_t inside = 0;
        for (std::uint32_t y = 0; y < 36; ++y) {
            for (std::uint32_t x = 0; x < 48; ++x) {
                if (radialRef(corner, {48, 36}, x, y) == 0.0) {
                    REQUIRE(sameColour(pixelOf(masked, x, y), pixelOf(reference, x, y)));
                    ++outside;
                } else if (!sameColour(pixelOf(masked, x, y), pixelOf(reference, x, y))) {
                    ++inside;
                }
            }
        }
        REQUIRE(outside > 1000);
        REQUIRE(inside > 10);
    }
}

TEST_CASE("Relative Temperature and Tint: the gains of ADR 044", "[local][balance]") {
    const auto near = [](const Colour& gain, float r, float g, float b) {
        return gain[0] == Catch::Approx(r).margin(1e-3) &&
               gain[1] == Catch::Approx(g).margin(1e-3) && gain[2] == Catch::Approx(b).margin(1e-3);
    };
    REQUIRE(near(relativeBalanceGainFor(100.0F, 0.0F), 1.100F, 0.985F, 0.731F));
    REQUIRE(near(relativeBalanceGainFor(-100.0F, 0.0F), 0.901F, 1.007F, 1.357F));
    REQUIRE(near(relativeBalanceGainFor(0.0F, 100.0F), 1.085F, 0.951F, 1.179F));
    REQUIRE(near(relativeBalanceGainFor(0.0F, -100.0F), 0.917F, 1.046F, 0.844F));
    for (const float temperature : {-100.0F, -37.0F, 12.0F, 100.0F}) {
        for (const float tint : {-100.0F, -5.0F, 60.0F}) {
            // A neutral keeps the luminance every later stage sees.
            REQUIRE(luminanceOf(relativeBalanceGainFor(temperature, tint)) ==
                    Catch::Approx(1.0F).margin(1e-6));
        }
    }
}

TEST_CASE("Relative Temperature warms and Tint moves towards magenta, in a render",
          "[local][balance]") {
    const ImageBuffer source = flat({8, 8}, 0.25F);
    const auto render = [&](const LocalDeltas& deltas) {
        return pixelOf(
            develop(source, withMask(DevelopState{plainSettings()}, everywhere(), deltas)), 3, 3);
    };
    const Colour warm = render({.relativeTemperature = 100.0F});
    REQUIRE(warm[0] > 0.25F);
    REQUIRE(warm[2] < 0.25F);
    const Colour cool = render({.relativeTemperature = -100.0F});
    REQUIRE(cool[0] < 0.25F);
    REQUIRE(cool[2] > 0.25F);
    const Colour magenta = render({.relativeTint = 100.0F});
    REQUIRE(magenta[1] < 0.25F);
    REQUIRE(magenta[0] > 0.25F);
    REQUIRE(magenta[2] > 0.25F);
    // A grey stays the luminance it was.
    for (const Colour& colour : {warm, cool, magenta}) {
        REQUIRE(luminanceOf(colour) == Catch::Approx(0.25F).epsilon(1e-6));
    }
    // And equals the law applied to the grey.
    const Colour gain = relativeBalanceGainFor(100.0F, 0.0F);
    REQUIRE(warm[0] == Catch::Approx(0.25F * gain[0]).epsilon(1e-6));
    REQUIRE(warm[2] == Catch::Approx(0.25F * gain[2]).epsilon(1e-6));
}

TEST_CASE("Zero relative Temperature and Tint apply no gain at all", "[local][balance]") {
    const ImageBuffer source = sceneOf({24, 16});
    const DevelopState without{plainSettings()};
    // Two masks whose temperature cancels at every pixel, and a tint of zero.
    DevelopState state = withMask(without, LinearMask{}, {.relativeTemperature = 40.0F});
    state = withMask(state, LinearMask{}, {.relativeTemperature = -40.0F});
    const PointwisePlan plan = planOf(source, state).pointwise;
    for (std::uint32_t y = 0; y < 16; ++y) {
        REQUIRE_FALSE(amountsAt(plan, 5, y).balances);
    }
    REQUIRE(sameBits(develop(source, state), develop(source, without)));
    // Two masks of 50 are one of 100.
    const DevelopState twice =
        withMask(withMask(without, LinearMask{}, {.relativeTemperature = 50.0F}), LinearMask{},
                 {.relativeTemperature = 50.0F});
    const DevelopState once = withMask(without, LinearMask{}, {.relativeTemperature = 100.0F});
    REQUIRE(sameBits(develop(source, twice), develop(source, once)));
}

namespace {

/// @brief The bases a plan prepares for given globals and one mask's deltas.
struct Bases {
    bool fine = false;
    bool coarse = false;
    bool floor = false;
    bool mean = false;
    friend bool operator==(const Bases&, const Bases&) = default;
};

Bases basesOf(const DevelopSettings& settings, const std::vector<LocalDeltas>& masks, float opacity,
              bool invert) {
    const ImageBuffer source = flat({64, 48});
    DevelopState state{settings};
    for (const LocalDeltas& deltas : masks) {
        state = withMask(state, LinearMask{}, deltas, opacity, invert);
    }
    const PresencePlan plan = planOf(source, state).pointwise.presence;
    return {plan.fine.active(), plan.coarse.active(), plan.hazeFloor.active(),
            plan.hazeMean.active()};
}

DevelopSettings presenceSettings(float texture, float clarity, float dehaze) {
    DevelopSettings settings = plainSettings();
    settings.presence = {.texture = texture, .clarity = clarity, .dehaze = dehaze};
    return settings;
}

} // namespace

TEST_CASE("The bases a plan prepares follow the reachable amounts", "[local][presence]") {
    struct Row {
        const char* what;
        DevelopSettings settings;
        std::vector<LocalDeltas> masks;
        float opacity;
        bool invert;
        Bases expected;
    };
    const std::vector<Row> rows{
        {"nothing", presenceSettings(0, 0, 0), {}, 1, false, {}},
        {"global texture alone",
         presenceSettings(30, 0, 0),
         {},
         1,
         false,
         {true, false, false, false}},
        {"global clarity alone",
         presenceSettings(0, -30, 0),
         {},
         1,
         false,
         {false, true, false, false}},
        {"global positive dehaze",
         presenceSettings(0, 0, 30),
         {},
         1,
         false,
         {false, false, true, false}},
        {"global negative dehaze",
         presenceSettings(0, 0, -30),
         {},
         1,
         false,
         {false, false, false, true}},
        {"local texture, global zero",
         presenceSettings(0, 0, 0),
         {{.texture = 20}},
         1,
         false,
         {true, false, false, false}},
        {"local clarity",
         presenceSettings(0, 0, 0),
         {{.clarity = -20}},
         1,
         false,
         {false, true, false, false}},
        {"local positive dehaze",
         presenceSettings(0, 0, 0),
         {{.dehaze = 20}},
         1,
         false,
         {false, false, true, false}},
        {"local negative dehaze",
         presenceSettings(0, 0, 0),
         {{.dehaze = -20}},
         1,
         false,
         {false, false, false, true}},
        {"both signs from two masks",
         presenceSettings(0, 0, 0),
         {{.dehaze = 20}, {.dehaze = -20}},
         1,
         false,
         {false, false, true, true}},
        {"both signs from a global and a mask",
         presenceSettings(0, 0, 30),
         {{.dehaze = -50}},
         1,
         false,
         {false, false, true, true}},
        {"a mask that cannot reach the other sign",
         presenceSettings(0, 0, 30),
         {{.dehaze = -20}},
         1,
         false,
         {false, false, true, false}},
        {"a mask that reaches exactly zero",
         presenceSettings(0, 0, -50),
         {{.dehaze = 50}},
         1,
         false,
         {false, false, false, true}},
        {"a mask that passes zero by the least",
         presenceSettings(0, 0, -50),
         {{.dehaze = 51}},
         1,
         false,
         {false, false, true, true}},
        {"opacity scales the reach",
         presenceSettings(0, 0, -50),
         {{.dehaze = 80}},
         0.5F,
         false,
         {false, false, false, true}},
        {"opacity scales the reach, past zero",
         presenceSettings(0, 0, -50),
         {{.dehaze = 80}},
         0.7F,
         false,
         {false, false, true, true}},
        {"inverting changes nothing",
         presenceSettings(0, 0, -50),
         {{.dehaze = 80}},
         0.7F,
         true,
         {false, false, true, true}},
        {"the clamp bounds the reach",
         presenceSettings(0, 0, 90),
         {{.dehaze = -150}},
         1,
         false,
         {false, false, true, true}},
        {"controls that cannot reach a base",
         presenceSettings(0, 0, 0),
         {{.exposure = 1, .contrast = 50, .saturation = 30}},
         1,
         false,
         {}},
        {"cancelling masks keep the base the sums reach",
         presenceSettings(0, 0, 0),
         {{.texture = 40}, {.texture = -40}},
         1,
         false,
         {true, false, false, false}},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        REQUIRE(basesOf(row.settings, row.masks, row.opacity, row.invert) == row.expected);
    }
}

TEST_CASE("Disabled masks, and masks of no amount, prepare no base", "[local][presence]") {
    const ImageBuffer source = flat({64, 48});
    DevelopState state{plainSettings()};
    state = withMask(state, LinearMask{}, {.texture = 40.0F, .dehaze = 30.0F});
    state = withLocalAdjustmentEnabled(state, state.localAdjustments[0].id, false);
    state = withMask(state, LinearMask{}, {.clarity = 40.0F}, 0.0F);
    REQUIRE(planOf(source, state).pointwise.presence == PresencePlan{});
}

TEST_CASE("A pixel reads a base only where it exists and its amount is not zero",
          "[local][presence]") {
    // A plan with Texture's and Dehaze's floor only; every amount asked for.
    PresencePlan plan;
    plan.lumaRow = colorspaces::workingLuminance;
    plan.fine = {.reduction = 1, .sigma = 1.0F, .radius = 3};
    plan.hazeFloor = {.reduction = 1, .sigma = 1.0F, .radius = 3, .window = 2};
    const Colour colour{0.3F, 0.2F, 0.1F};
    const PixelContext context{.fineBase = -3.0F,
                               .coarseBase = -5.0F,
                               .coarseCell = -1.0F,
                               .hazeFloor = -4.0F,
                               .hazeMean = -1.0F};
    const float logLuminance = -2.0F;
    const auto apply = [&](float texture, float clarity, float dehaze) {
        return applyPresence(plan, {texture, clarity, dehaze}, colour, logLuminance, context);
    };
    // Clarity has no base: it contributes nothing, whatever its amount.
    REQUIRE(sameColour(apply(0.0F, 0.7F, 0.0F), colour));
    // A negative Dehaze has no mean to read: nothing.
    REQUIRE(sameColour(apply(0.0F, 0.0F, -0.4F), colour));
    // A rounding error past zero reads nothing either.
    REQUIRE(sameColour(apply(0.0F, 0.0F, -1e-9F), colour));
    // A zero amount reads nothing, with its base present.
    REQUIRE(sameColour(apply(0.0F, 0.0F, 0.0F), colour));
    // The bases that exist act.
    REQUIRE_FALSE(sameColour(apply(0.5F, 0.0F, 0.0F), colour));
    REQUIRE_FALSE(sameColour(apply(0.0F, 0.0F, 0.5F), colour));
    // And the two together are Texture's gain and then the floor's.
    REQUIRE_FALSE(sameColour(apply(0.5F, 0.7F, 0.5F), apply(0.5F, 0.0F, 0.5F)) == false);

    // The mean, present alone, reads for a negative amount and never a positive one.
    PresencePlan meanOnly;
    meanOnly.lumaRow = colorspaces::workingLuminance;
    meanOnly.hazeMean = {.reduction = 1, .sigma = 1.0F, .radius = 3};
    REQUIRE_FALSE(sameColour(
        applyPresence(meanOnly, {0.0F, 0.0F, -0.4F}, colour, logLuminance, context), colour));
    REQUIRE(sameColour(applyPresence(meanOnly, {0.0F, 0.0F, 0.4F}, colour, logLuminance, context),
                       colour));
    REQUIRE(sameColour(applyPresence(meanOnly, {0.0F, 0.0F, 1e-9F}, colour, logLuminance, context),
                       colour));
}

namespace {

/// @brief A scene with a hazy lower half and a clear upper half, in colour, for Dehaze.
ImageBuffer veiledScene(ImageSize size) {
    return imageOf(size, [size](std::uint32_t x, std::uint32_t y) {
        const Colour base = sceneColour(x, y);
        const float veil = 0.25F * static_cast<float>(y) / static_cast<float>(size.height);
        return Colour{base[0] * 0.6F + veil, base[1] * 0.6F + veil, base[2] * 0.6F + veil};
    });
}

/// @brief A radial mask over the left half of a frame, and one over the right.
RadialMask leftBlob() {
    return {
        .centre = {0.25F, 0.5F}, .radiusX = 0.2F, .radiusY = 0.2F, .angle = 0.0F, .feather = 0.5F};
}

RadialMask rightBlob() {
    return {
        .centre = {0.75F, 0.5F}, .radiusX = 0.2F, .radiusY = 0.2F, .angle = 0.0F, .feather = 0.5F};
}

} // namespace

TEST_CASE("Mixed-sign Dehaze across one frame is two single-sign renders composed",
          "[local][presence]") {
    const ImageSize size{96, 64};
    const ImageBuffer source = veiledScene(size);
    const DevelopState none{plainSettings()};
    const DevelopState positive = withMask(none, leftBlob(), {.dehaze = 70.0F});
    const DevelopState negative = withMask(none, rightBlob(), {.dehaze = -70.0F});
    const DevelopState both = withMask(positive, rightBlob(), {.dehaze = -70.0F});

    const PresencePlan plan = planOf(source, both).pointwise.presence;
    REQUIRE(plan.hazeFloor.active());
    REQUIRE(plan.hazeMean.active());

    const ImageBuffer mixed = develop(source, both);
    const ImageBuffer floorOnly = develop(source, positive);
    const ImageBuffer meanOnly = develop(source, negative);
    const ImageBuffer plain = develop(source, none);
    std::size_t floorPixels = 0;
    std::size_t meanPixels = 0;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const bool left = radialRef(leftBlob(), size, x, y) > 0.0;
            const bool right = radialRef(rightBlob(), size, x, y) > 0.0;
            if (left && !right) {
                REQUIRE(sameColour(pixelOf(mixed, x, y), pixelOf(floorOnly, x, y)));
                floorPixels += !sameColour(pixelOf(mixed, x, y), pixelOf(plain, x, y)) ? 1 : 0;
            } else if (right && !left) {
                REQUIRE(sameColour(pixelOf(mixed, x, y), pixelOf(meanOnly, x, y)));
                meanPixels += !sameColour(pixelOf(mixed, x, y), pixelOf(plain, x, y)) ? 1 : 0;
            } else if (!left && !right) {
                REQUIRE(sameColour(pixelOf(mixed, x, y), pixelOf(plain, x, y)));
            }
        }
    }
    REQUIRE(floorPixels > 200);
    REQUIRE(meanPixels > 200);
}

TEST_CASE("Masks that cancel Dehaze to zero read neither base", "[local][presence]") {
    const ImageSize size{96, 64};
    const ImageBuffer source = veiledScene(size);
    const DevelopState none{plainSettings()};
    DevelopState cancelling = withMask(none, leftBlob(), {.dehaze = 50.0F});
    cancelling = withMask(cancelling, leftBlob(), {.dehaze = -50.0F});
    const PresencePlan plan = planOf(source, cancelling).pointwise.presence;
    REQUIRE(plan.hazeFloor.active());
    REQUIRE(plan.hazeMean.active());
    REQUIRE(sameBits(develop(source, cancelling), develop(source, none)));
}

TEST_CASE("Presence under masks alone, with every global at zero", "[local][presence]") {
    const ImageSize size{96, 64};
    const ImageBuffer source = veiledScene(size);
    const DevelopState none{plainSettings()};
    const ImageBuffer plain = develop(source, none);
    const LocalDeltas deltas{.texture = 60.0F, .clarity = 50.0F, .dehaze = 40.0F};
    const ImageBuffer masked = develop(source, withMask(none, leftBlob(), deltas));
    // The same render as the global controls at those amounts, where the mask is one.
    const ImageBuffer global = develop(source, DevelopState{presenceSettings(60.0F, 50.0F, 40.0F)});
    std::size_t changed = 0;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double weight = radialRef(leftBlob(), size, x, y);
            if (weight == 0.0) {
                REQUIRE(sameColour(pixelOf(masked, x, y), pixelOf(plain, x, y)));
            } else if (weight == 1.0) {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    REQUIRE(
                        pixelOf(masked, x, y)[channel] ==
                        Catch::Approx(pixelOf(global, x, y)[channel]).epsilon(1e-5).margin(1e-7));
                }
                ++changed;
            }
        }
    }
    REQUIRE(changed > 100);
    REQUIRE_FALSE(sameBits(masked, plain));
}

TEST_CASE("A global positive Dehaze under a negative mask, and the exact boundary",
          "[local][presence]") {
    const ImageSize size{96, 64};
    const ImageBuffer source = veiledScene(size);

    SECTION("the mask passes through zero to a negative amount") {
        DevelopSettings settings = presenceSettings(0.0F, 0.0F, 40.0F);
        const DevelopState state =
            withMask(DevelopState{settings}, everywhere(), {.dehaze = -70.0F});
        const PresencePlan plan = planOf(source, state).pointwise.presence;
        REQUIRE(plan.hazeFloor.active());
        REQUIRE(plan.hazeMean.active());
        const ImageBuffer masked = develop(source, state);
        const ImageBuffer global =
            develop(source, DevelopState{presenceSettings(0.0F, 0.0F, -30.0F)});
        REQUIRE(largestDifference(masked, global) <= 1e-6F);
    }
    SECTION("an interval that ends exactly at zero prepares no floor and reads none") {
        DevelopSettings settings = presenceSettings(0.0F, 0.0F, -50.0F);
        const DevelopState state =
            withMask(DevelopState{settings}, everywhere(), {.dehaze = 50.0F});
        const PresencePlan plan = planOf(source, state).pointwise.presence;
        REQUIRE_FALSE(plan.hazeFloor.active());
        REQUIRE(plan.hazeMean.active());
        const ImageBuffer masked = develop(source, state);
        const ImageBuffer none = develop(source, DevelopState{presenceSettings(0.0F, 0.0F, 0.0F)});
        REQUIRE(largestDifference(masked, none) <= 1e-6F);
        // And a mask of half the weight is a negative amount: the mean.
        const DevelopState half = withMask(DevelopState{settings}, everywhere(), {.dehaze = 25.0F});
        const ImageBuffer halved = develop(source, half);
        const ImageBuffer global =
            develop(source, DevelopState{presenceSettings(0.0F, 0.0F, -25.0F)});
        REQUIRE(largestDifference(halved, global) <= 1e-6F);
    }
}

TEST_CASE("A mask reaches Tone through the chain: a control the mask has and the global does not",
          "[local][tone]") {
    const ImageBuffer source = sceneOf({32, 24});
    // No global tone shaping, so the plan does not shape tone; a mask with Highlights makes only
    // the pixels it reaches do so.
    const DevelopState none{plainSettings()};
    const RadialMask blob{
        .centre = {0.5F, 0.5F}, .radiusX = 0.2F, .radiusY = 0.2F, .angle = 0.0F, .feather = 0.0F};
    const DevelopState state = withMask(none, blob, {.highlights = -80.0F});
    const PointwisePlan plan = planOf(source, state).pointwise;
    REQUIRE_FALSE(plan.tone.shapesTone);
    REQUIRE(amountsAt(plan, 16, 12).tone.shapesTone);
    REQUIRE_FALSE(amountsAt(plan, 0, 0).tone.shapesTone);
    // A pixel no mask reaches has exactly the plan's amounts.
    REQUIRE(amountsAt(plan, 0, 0) == globalAmountsOf(plan));
}

TEST_CASE("The mask stays on the same source pixels under every orientation", "[local][anchor]") {
    const ImageSize size{37, 23};
    const DevelopState state = withMask(
        withMask(DevelopState{plainSettings()}, LinearMask{{0.1F, 0.2F}, {0.6F, 0.9F}},
                 {.relativeTemperature = 30.0F, .exposure = 1.0F}),
        RadialMask{{0.7F, 0.3F}, 0.2F, 0.1F, 20.0F, 0.3F}, {.exposure = -1.0F, .contrast = 40.0F});
    const DevelopState none{plainSettings()};
    const ImageBuffer normal = sceneOf(size);
    const ImageBuffer upright = develop(normal, state);

    // Where the source pixel (x, y) of a W x H frame lands under an EXIF orientation.
    const auto landing = [](ImageOrientation orientation, ImageSize frame, std::uint32_t x,
                            std::uint32_t y) {
        const std::uint32_t w = frame.width - 1;
        const std::uint32_t h = frame.height - 1;
        switch (orientation) {
        case ImageOrientation::Normal:
            return std::pair{x, y};
        case ImageOrientation::MirrorHorizontal:
            return std::pair{w - x, y};
        case ImageOrientation::Rotate180:
            return std::pair{w - x, h - y};
        case ImageOrientation::MirrorVertical:
            return std::pair{x, h - y};
        case ImageOrientation::Transpose:
            return std::pair{y, x};
        case ImageOrientation::Rotate90:
            return std::pair{h - y, x};
        case ImageOrientation::Transverse:
            return std::pair{h - y, w - x};
        case ImageOrientation::Rotate270:
            return std::pair{y, w - x};
        }
        return std::pair{x, y};
    };

    for (const auto orientation :
         {ImageOrientation::Normal, ImageOrientation::MirrorHorizontal, ImageOrientation::Rotate180,
          ImageOrientation::MirrorVertical, ImageOrientation::Transpose, ImageOrientation::Rotate90,
          ImageOrientation::Transverse, ImageOrientation::Rotate270}) {
        INFO("orientation " << static_cast<int>(orientation));
        const ImageBuffer turned = sceneOf(size, orientation);
        const ImageBuffer render = develop(turned, state);
        const ImageBuffer plainRender = develop(turned, none);
        // The orientation itself, found out on the render without masks: the oriented pixel is
        // the source pixel where the orientation lands it.
        for (std::uint32_t y = 0; y < size.height; ++y) {
            for (std::uint32_t x = 0; x < size.width; ++x) {
                const auto [ox, oy] = landing(orientation, size, x, y);
                REQUIRE(
                    largestDifference(
                        imageOf({1, 1}, [&](auto, auto) { return pixelOf(plainRender, ox, oy); }),
                        imageOf({1, 1}, [&](auto, auto) { return sceneColour(x, y); })) <= 1e-6F);
                // The masked render at the landing place is the masked upright render.
                const Colour got = pixelOf(render, ox, oy);
                const Colour want = pixelOf(upright, x, y);
                REQUIRE(got[0] == Catch::Approx(want[0]).epsilon(1e-5).margin(1e-6));
                REQUIRE(got[1] == Catch::Approx(want[1]).epsilon(1e-5).margin(1e-6));
                REQUIRE(got[2] == Catch::Approx(want[2]).epsilon(1e-5).margin(1e-6));
            }
        }
    }
}

TEST_CASE("The mask stays on the same source pixels under crop, straighten, turns and flips",
          "[local][anchor]") {
    const ImageSize size{80, 50};
    const ImageBuffer source = sceneOf(size);
    const DevelopState masked =
        withMask(withMask(DevelopState{plainSettings()}, LinearMask{{0.1F, 0.2F}, {0.6F, 0.9F}},
                          {.exposure = 1.0F, .highlights = -40.0F}),
                 RadialMask{{0.7F, 0.3F}, 0.2F, 0.1F, 20.0F, 0.3F}, {.exposure = -1.0F});

    // The pointwise result is the same whatever the geometry: the checkpoint after it is.
    const ImageBuffer pointwise = developUntil(source, masked, Stage::Pointwise).readBack();
    std::vector<GeometrySettings> geometries;
    geometries.push_back({.straighten = 7.5});
    geometries.push_back({.rotation = QuarterTurn::Clockwise90});
    geometries.push_back({.rotation = QuarterTurn::Clockwise270, .flipHorizontal = true});
    geometries.push_back({.flipHorizontal = true, .flipVertical = true});
    GeometrySettings cropped;
    cropped.straighten = -4.0;
    cropped.rotation = QuarterTurn::Clockwise180;
    cropped.crop.rectangle = UprightCropRect{0.2, 0.15, 0.85, 0.9};
    geometries.push_back(cropped);
    for (const GeometrySettings& geometry : geometries) {
        DevelopState state = masked;
        state.settings.geometry = geometry;
        // Pointwise, then geometry on the result: what a render is.
        REQUIRE(sameBits(developUntil(source, state, Stage::Pointwise).readBack(), pointwise));
        DevelopState onlyGeometry{plainSettings()};
        onlyGeometry.settings.geometry = geometry;
        REQUIRE(sameBits(develop(source, state), develop(pointwise, onlyGeometry)));
    }
}

TEST_CASE("A reduced level evaluates the same field as the full source", "[local][level]") {
    const DevelopState state =
        withMask(withMask(DevelopState{plainSettings()}, LinearMask{{0.1F, 0.2F}, {0.6F, 0.9F}},
                          {.exposure = 1.0F}),
                 RadialMask{{0.7F, 0.3F}, 0.3F, 0.2F, 20.0F, 0.5F},
                 {.exposure = -1.0F, .saturation = 40.0F});
    for (const std::uint32_t factor : {2U, 4U}) {
        const ImageSize full{160, 96};
        const ImageSize reduced{full.width / factor, full.height / factor};
        const LocalPlan big = localPlanFor(state, full);
        const LocalPlan small = localPlanFor(state, reduced);
        for (std::size_t index = 0; index < big.masks.size(); ++index) {
            for (std::uint32_t y = 0; y < reduced.height; ++y) {
                for (std::uint32_t x = 0; x < reduced.width; ++x) {
                    // The centre of a reduced pixel is the corner shared by factor x factor pixels.
                    const float fx = (static_cast<float>(x) + 0.5F) * static_cast<float>(factor);
                    const float fy = (static_cast<float>(y) + 0.5F) * static_cast<float>(factor);
                    REQUIRE(maskWeight(small.masks[index], static_cast<float>(x) + 0.5F,
                                       static_cast<float>(y) + 0.5F) ==
                            Catch::Approx(maskWeight(big.masks[index], fx, fy)).margin(3e-5));
                }
            }
        }
    }

    // And the renders agree where the field is smooth: the reduced render is the mean of the
    // full render's blocks.
    const ImageBuffer fullSource = flat({128, 64}, 0.2F);
    ImageBuffer reducedSource = flat({64, 32}, 0.2F);
    reducedSource.setPixelScale(2.0);
    const ImageBuffer big = develop(fullSource, state);
    const ImageBuffer small = develop(reducedSource, state);
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            double mean = 0.0;
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    mean += pixelOf(big, 2 * x + dx, 2 * y + dy)[1] / 4.0;
                }
            }
            REQUIRE(pixelOf(small, x, y)[1] == Catch::Approx(mean).epsilon(1e-2));
        }
    }
}

TEST_CASE("A region render evaluates the same field as the whole render", "[local][region]") {
    const ImageSize size{64, 48};
    const ImageBuffer source = sceneOf(size);
    DevelopSettings settings = plainSettings();
    settings.geometry.straighten = 3.0;
    const DevelopState state = withMask(
        withMask(DevelopState{settings}, LinearMask{{0.1F, 0.2F}, {0.6F, 0.9F}},
                 {.exposure = 1.0F, .texture = 30.0F}),
        RadialMask{{0.7F, 0.3F}, 0.3F, 0.2F, 20.0F, 0.5F}, {.exposure = -1.0F, .dehaze = 30.0F});
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

TEST_CASE("A mask edit resumes from the Denoise checkpoint and never reruns noise reduction",
          "[local][ladder]") {
    const auto source = std::make_shared<const ImageBuffer>(sceneOf({64, 40}));
    DevelopSettings settings = plainSettings();
    settings.noiseReduction.color = 40.0F;
    settings.noiseReduction.luminance = 30.0F;
    DevelopState state{settings};
    state = withMask(state, LinearMask{}, {.exposure = 1.0F});

    CheckpointLadder ladder;
    const LadderRender first = resumeOrDevelop(ladder, source, state, {});
    REQUIRE_FALSE(first.resumedFrom.has_value());
    REQUIRE(ladder.holds(Stage::Denoise));

    const std::vector<std::pair<const char*, DevelopState>> edits{
        {"delta", withLocalDelta(state, state.localAdjustments[0].id, "exposure", 2.0)},
        {"shape", withLocalShape(state, state.localAdjustments[0].id,
                                 LinearMask{{0.5F, 0.1F}, {0.5F, 0.9F}})},
        {"invert", withLocalAdjustmentInverted(state, state.localAdjustments[0].id, true)},
        {"disable", withLocalAdjustmentEnabled(state, state.localAdjustments[0].id, false)},
        {"add", withMask(state, RadialMask{}, {.dehaze = 20.0F})},
        {"remove", withLocalAdjustmentRemoved(state, state.localAdjustments[0].id)},
    };
    for (const auto& [what, edited] : edits) {
        INFO(what);
        CheckpointLadder resumed;
        static_cast<void>(resumeOrDevelop(resumed, source, state, {}));
        const LadderRender render = resumeOrDevelop(resumed, source, edited, {});
        REQUIRE(render.resumedFrom == Stage::Denoise);
        REQUIRE(sameBits(render.checkpoint.readBack(), develop(*source, edited)));
    }
}

TEST_CASE("A mask edit invalidates the Pointwise checkpoint and nothing before it",
          "[local][ladder]") {
    const ImageBuffer source = sceneOf({32, 24});
    DevelopSettings settings = plainSettings();
    settings.noiseReduction.color = 40.0F;
    const DevelopState state = withMask(DevelopState{settings}, LinearMask{}, {.exposure = 1.0F});
    const DevelopState edited =
        withLocalDelta(state, state.localAdjustments[0].id, "exposure", 1.5);
    const ProcessingPlan before = planOf(source, state);
    const ProcessingPlan after = planOf(source, edited);
    REQUIRE(prefixMatches(before, after, Stage::Denoise));
    REQUIRE_FALSE(prefixMatches(before, after, Stage::Pointwise));
    REQUIRE_FALSE(prefixMatches(before, after, Stage::Effects));
}

TEST_CASE("The curve-input tap follows the local controls before it, and not the ones after",
          "[local][tap]") {
    const ImageBuffer source = sceneOf({48, 32});
    const DevelopState none{plainSettings()};
    const LinearMask ramp{{0.5F, 0.1F}, {0.5F, 0.9F}};
    const auto same = [&](const DevelopState& a, const DevelopState& b) {
        return sameAtTap(planOf(source, a), planOf(source, b), Tap::CurveInput);
    };

    DevelopState exposure = withMask(none, ramp, {.exposure = 1.0F});
    DevelopState saturation = withMask(none, ramp, {.saturation = 30.0F});

    SECTION("a local exposure change moves the tap") {
        const DevelopState changed =
            withLocalDelta(exposure, exposure.localAdjustments[0].id, "exposure", 1.5);
        REQUIRE_FALSE(same(exposure, changed));
        REQUIRE_FALSE(same(none, exposure));
    }
    SECTION("each of the eleven controls before the tap moves it") {
        for (std::size_t row = 0; row < preTapControlCount; ++row) {
            const LocalDescriptor& descriptor = localAdjustmentDescriptors[row];
            INFO(descriptor.key);
            LocalDeltas deltas;
            deltas.*descriptor.member = 1.0F;
            const DevelopState first = withMask(none, ramp, deltas);
            const DevelopState second = withLocalDelta(first, first.localAdjustments[0].id,
                                                       std::string(descriptor.key), 3.0);
            REQUIRE_FALSE(same(first, second));
        }
    }
    SECTION("a local saturation or vibrance change leaves it") {
        const DevelopState changed =
            withLocalDelta(saturation, saturation.localAdjustments[0].id, "saturation", 60.0);
        REQUIRE(same(saturation, changed));
        REQUIRE(same(none, saturation));
        const DevelopState vibrant =
            withLocalDelta(saturation, saturation.localAdjustments[0].id, "vibrance", 20.0);
        REQUIRE(same(saturation, vibrant));
    }
    SECTION("moving a mask that carries only those leaves it") {
        const DevelopState moved = withLocalShape(saturation, saturation.localAdjustments[0].id,
                                                  LinearMask{{0.2F, 0.3F}, {0.9F, 0.4F}});
        REQUIRE(same(saturation, moved));
        const DevelopState inverted =
            withLocalAdjustmentInverted(saturation, saturation.localAdjustments[0].id, true);
        REQUIRE(same(saturation, inverted));
        // Nor does reordering it among others of its kind.
        DevelopState two = withMask(saturation, RadialMask{}, {.vibrance = 10.0F});
        const DevelopState reordered =
            withLocalAdjustmentReordered(two, two.localAdjustments[1].id, 0);
        REQUIRE(same(two, reordered));
    }
    SECTION("a mask with both kinds is seen by its pre-tap controls alone") {
        const DevelopState mixed = withMask(none, ramp, {.exposure = 1.0F, .saturation = 10.0F});
        const DevelopState moreColour =
            withLocalDelta(mixed, mixed.localAdjustments[0].id, "saturation", 70.0);
        REQUIRE(same(mixed, moreColour));
        const DevelopState moreLight =
            withLocalDelta(mixed, mixed.localAdjustments[0].id, "exposure", 2.0);
        REQUIRE_FALSE(same(mixed, moreLight));
    }
    SECTION("a pre-tap mask's geometry, invert, order and opacity move it") {
        DevelopState two =
            withMask(withMask(none, ramp, {.exposure = 1.0F}), RadialMask{}, {.highlights = 30.0F});
        const LocalAdjustmentId first = two.localAdjustments[0].id;
        const LocalAdjustmentId second = two.localAdjustments[1].id;
        REQUIRE_FALSE(
            same(two, withLocalShape(two, first, LinearMask{{0.5F, 0.2F}, {0.5F, 0.8F}})));
        REQUIRE_FALSE(same(two, withLocalAdjustmentInverted(two, first, true)));
        REQUIRE_FALSE(same(two, withLocalAdjustmentReordered(two, second, 0)));
        REQUIRE_FALSE(same(two, withLocalOpacity(two, first, 0.5F)));
        REQUIRE_FALSE(same(two, withLocalAdjustmentEnabled(two, first, false)));
        REQUIRE(same(two, two));
    }
    SECTION("a global change that the masks add to moves it, and one they do not read does not") {
        DevelopSettings colour = plainSettings();
        colour.color.saturation = 20.0F;
        DevelopState other = exposure;
        other.settings = colour;
        REQUIRE(same(exposure, other));
        DevelopSettings brighter = plainSettings();
        brighter.tone.exposure = 0.5F;
        DevelopState brightest = exposure;
        brightest.settings = brighter;
        REQUIRE_FALSE(same(exposure, brightest));
    }
}

TEST_CASE("A saturation mask is seen after the tap, in a render of the tap's sample",
          "[local][tap]") {
    const ImageBuffer source = sceneOf({32, 24});
    const DevelopState none{plainSettings()};
    const DevelopState saturated = withMask(none, everywhere(), {.saturation = 80.0F});
    const DevelopState lit = withMask(none, everywhere(), {.exposure = 1.0F});
    REQUIRE(sameBits(sample(source, saturated, Tap::CurveInput),
                     sample(source, none, Tap::CurveInput)));
    REQUIRE_FALSE(
        sameBits(sample(source, lit, Tap::CurveInput), sample(source, none, Tap::CurveInput)));
    // The tap's sample is the chain's prefix at the pixel's own amounts.
    const ImageBuffer tap = sample(source, lit, Tap::CurveInput);
    const ImageBuffer plain = sample(source, none, Tap::CurveInput);
    REQUIRE(pixelOf(tap, 4, 4)[1] > pixelOf(plain, 4, 4)[1]);
}

TEST_CASE("The pre-tap view holds the masks with a pre-tap control and the eleven globals",
          "[local][tap]") {
    const ImageBuffer source = flat({16, 16});
    DevelopSettings settings = plainSettings();
    settings.tone.exposure = 0.5F;
    settings.color.saturation = 33.0F;
    DevelopState state{settings};
    state = withMask(state, LinearMask{}, {.saturation = 10.0F});
    REQUIRE(preTapLocalFieldsOf(planOf(source, state).pointwise.local) == PreTapLocal{});
    state = withMask(state, RadialMask{}, {.exposure = 1.0F, .vibrance = 5.0F});
    const PreTapLocal view = preTapLocalFieldsOf(planOf(source, state).pointwise.local);
    REQUIRE(view.masks.size() == 1);
    REQUIRE(view.masks[0].kind == LocalMaskKind::Radial);
    REQUIRE(view.masks[0].k[indexOf(LocalControl::Exposure)] == 1.0F);
    REQUIRE(view.global[indexOf(LocalControl::Exposure)] == 0.5F);
}
