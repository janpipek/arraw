#include "ProcessingPlan.h"
#include "ToneCurve.h"

#include <DevelopSettings.h>
#include <DevelopState.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace arraw;
using Catch::Approx;

/// The tone curves of ADR 011: control points resolved into a table by monotone
/// cubic interpolation, applied after Basic Tone and before the shoulder.

namespace {

/// @brief Builds a curve from its points.
ToneCurve curveOf(std::vector<CurvePoint> points) {
    return ToneCurve{std::move(points)};
}

/// @brief Resolves settings into a plan, with the shoulder and colour block out of the way.
ProcessingPlan planOf(DevelopSettings settings) {
    settings.tone.filmicHighlights = noFilmicHighlights;
    return planFor(ColorEncoding{workingEncoding}, DevelopState{settings});
}

/// @brief Whether two colours have the same bits in every channel, NaN included.
bool sameBits(Colour first, Colour second) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::bit_cast<std::uint32_t>(first[i]) != std::bit_cast<std::uint32_t>(second[i])) {
            return false;
        }
    }
    return true;
}

/// @brief Luminance of a colour in the working encoding.
float luminanceOf(Colour colour) {
    return colorspaces::workingLuminance[0] * colour[0] +
           colorspaces::workingLuminance[1] * colour[1] +
           colorspaces::workingLuminance[2] * colour[2];
}

/// @brief Colours across the awkward places: black, neutral, vivid, above white, negative, NaN.
std::vector<Colour> awkwardColours() {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    return {{0.0F, 0.0F, 0.0F}, {0.18F, 0.18F, 0.18F}, {1.0F, 1.0F, 1.0F},
            {1.0F, 0.0F, 0.0F}, {0.2F, 0.6F, 0.1F},    {0.05F, 0.3F, 0.9F},
            {4.0F, 2.0F, 0.5F}, {-0.1F, 0.4F, 0.2F},   {1.0e-9F, 0.0F, 3.0e-9F},
            {nan, 0.5F, 0.5F},  {inf, 0.0F, 0.0F},     {0.25F, 0.5F, 0.7F}};
}

/// @brief A curve that lifts shadows and holds highlights, to see what it does.
const ToneCurve sCurve = curveOf({{0.0F, 0.0F}, {0.25F, 0.15F}, {0.75F, 0.85F}, {1.0F, 1.0F}});

} // namespace

TEST_CASE("An identity curve is off and the default plan leaves every colour bit for bit alone",
          "[tonecurve]") {
    const auto plan = planOf({});
    REQUIRE(plan.pointwise.toneCurves == ToneCurvePlan{});
    REQUIRE_FALSE(plan.pointwise.toneCurves.luma.active);
    REQUIRE_FALSE(plan.pointwise.toneCurves.red.active);
    REQUIRE_FALSE(plan.pointwise.toneCurves.green.active);
    REQUIRE_FALSE(plan.pointwise.toneCurves.blue.active);
    REQUIRE_FALSE(curvePlanFor(curveOf({{0.0F, 0.0F}, {1.0F, 1.0F}})).active);

    for (const Colour colour : awkwardColours()) {
        CAPTURE(colour[0], colour[1], colour[2]);
        REQUIRE(sameBits(applyToneCurves(plan.pointwise.toneCurves, colour), colour));
        // The chain as it was before the curves: matrix, gain, tone, shoulder, colour.
        Colour expected = plan.pointwise.toWorking * colour;
        expected = {expected[0] * plan.pointwise.tone.exposureGain,
                    expected[1] * plan.pointwise.tone.exposureGain,
                    expected[2] * plan.pointwise.tone.exposureGain};
        expected = shapeTone(plan.pointwise.tone, expected);
        expected = rollHighlights(plan.pointwise.shoulderKnee, expected);
        expected = adjustColor(plan.pointwise.colorAdjustments, expected);
        REQUIRE(sameBits(developPixel(plan.pointwise, colour), expected));
    }
}

TEST_CASE("The resolved curve passes through its control points", "[tonecurve]") {
    const std::vector<ToneCurve> curves{
        sCurve,
        curveOf({{0.0F, 0.2F}, {0.5F, 0.5F}, {1.0F, 0.9F}}),
        curveOf({{0.0F, 0.0F}, {0.1F, 0.8F}, {0.2F, 0.82F}, {0.9F, 0.9F}, {1.0F, 1.0F}}),
        curveOf({{0.0F, 1.0F}, {1.0F, 0.0F}}),
    };
    for (const ToneCurve& curve : curves) {
        const CurvePlan plan = curvePlanFor(curve);
        REQUIRE(plan.active);
        for (const CurvePoint& point : curve.points) {
            CAPTURE(point.x, point.y);
            REQUIRE(evaluateCurve(plan, point.x) == Approx(point.y).margin(5.0e-4));
        }
        // The ends are the points themselves.
        REQUIRE(evaluateCurve(plan, 0.0F) == curve.points.front().y);
        REQUIRE(evaluateCurve(plan, 1.0F) == curve.points.back().y);
    }
}

TEST_CASE("Rising points give a rising table that never overshoots them", "[tonecurve]") {
    // Steep then flat then steep: the case where a natural spline rings.
    const ToneCurve curve = curveOf(
        {{0.0F, 0.0F}, {0.1F, 0.8F}, {0.2F, 0.82F}, {0.3F, 0.83F}, {0.9F, 0.9F}, {1.0F, 1.0F}});
    const CurvePlan plan = curvePlanFor(curve);
    for (std::size_t i = 1; i < toneCurveSamples; ++i) {
        REQUIRE(plan.table[i] >= plan.table[i - 1]);
    }
    REQUIRE(plan.table.front() == 0.0F);
    REQUIRE(plan.table.back() == 1.0F);

    // Within every segment the table stays between that segment's two points.
    for (std::size_t i = 0; i < toneCurveSamples; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(toneCurveSamples - 1);
        const auto upper = std::ranges::find_if(
            curve.points, [x](const CurvePoint& point) { return point.x >= x; });
        const auto lower = upper == curve.points.begin() ? upper : std::prev(upper);
        REQUIRE(plan.table[i] >= lower->y - 1.0e-6F);
        REQUIRE(plan.table[i] <= upper->y + 1.0e-6F);
    }
}

TEST_CASE("A curve that turns round does not overshoot its turning point", "[tonecurve]") {
    const CurvePlan plan = curvePlanFor(curveOf({{0.0F, 0.0F}, {0.5F, 0.9F}, {1.0F, 0.2F}}));
    const float peak = *std::ranges::max_element(plan.table);
    REQUIRE(peak <= 0.9F + 1.0e-6F);
    REQUIRE(peak >= 0.9F - 1.0e-3F);
    REQUIRE(*std::ranges::min_element(plan.table) >= 0.0F);
}

TEST_CASE("Above one the curve goes on at slope one, below zero it holds", "[tonecurve]") {
    // A steep last segment: slope 5 from (0.9, 0.5) to (1, 1). Above one the
    // curve does not follow it but rises one for one from curve(1).
    const CurvePlan steep = curvePlanFor(curveOf({{0.0F, 0.0F}, {0.9F, 0.5F}, {1.0F, 1.0F}}));
    REQUIRE(evaluateCurve(steep, 1.5F) == 1.5F);
    REQUIRE(evaluateCurve(steep, 4.0F) == 4.0F);

    const CurvePlan plan = curvePlanFor(curveOf({{0.0F, 0.1F}, {0.5F, 0.3F}, {1.0F, 0.8F}}));
    REQUIRE(evaluateCurve(plan, 1.5F) == Approx(1.3F));
    REQUIRE(evaluateCurve(plan, 4.0F) == Approx(3.8F));
    // Continuous at one: no step where the extension starts.
    REQUIRE(evaluateCurve(plan, std::nextafter(1.0F, 2.0F)) == Approx(0.8F).margin(1.0e-6F));

    REQUIRE(evaluateCurve(plan, 0.0F) == 0.1F);
    REQUIRE(evaluateCurve(plan, -0.5F) == 0.1F);
    REQUIRE(evaluateCurve(plan, -1.0e9F) == 0.1F);
    REQUIRE(evaluateCurve(plan, std::numeric_limits<float>::quiet_NaN()) == 0.1F);
}

TEST_CASE("A curve with a flat end keeps an infinite input infinite", "[tonecurve]") {
    constexpr float inf = std::numeric_limits<float>::infinity();
    const CurvePlan flat = curvePlanFor(curveOf({{0.0F, 0.0F}, {0.5F, 0.8F}, {1.0F, 0.8F}}));
    REQUIRE(evaluateCurve(flat, 2.0F) == Approx(1.8F));
    REQUIRE(evaluateCurve(flat, inf) == inf);

    DevelopSettings settings;
    settings.toneCurve.luma = curveOf({{0.0F, 0.0F}, {0.5F, 0.8F}, {1.0F, 0.8F}});
    settings.toneCurve.red = settings.toneCurve.luma;
    const auto plan = planOf(settings);
    const Colour result = applyToneCurves(plan.pointwise.toneCurves, {0.0F, 0.0F, 0.0F});
    REQUIRE(result == Colour{0.0F, 0.0F, 0.0F});
    // A channel curve on infinity gives infinity, not NaN.
    settings.toneCurve.luma = ToneCurve{};
    REQUIRE(applyToneCurves(planOf(settings).pointwise.toneCurves, {inf, 0.5F, 0.5F})[0] == inf);
}

TEST_CASE("A limited end tangent still meets the extension without a step", "[tonecurve]") {
    // The limiter cuts the tangent at 1 to about 0.008, far below the end
    // secant of 0.11; the extension does not use either, and starts at curve(1).
    const CurvePlan plan = curvePlanFor(curveOf({{0.0F, 0.0F}, {0.1F, 0.9F}, {1.0F, 1.0F}}));
    for (std::size_t i = 1; i < toneCurveSamples; ++i) {
        REQUIRE(plan.table[i] >= plan.table[i - 1]);
    }
    // Nearly flat into one: the last table step is a small fraction of the secant's.
    const float lastStep = plan.table[toneCurveSamples - 1] - plan.table[toneCurveSamples - 2];
    REQUIRE(lastStep * static_cast<float>(toneCurveSamples - 1) < 0.02F);
    REQUIRE(evaluateCurve(plan, std::nextafter(1.0F, 2.0F)) == Approx(1.0F).margin(1.0e-6F));
    REQUIRE(evaluateCurve(plan, 1.25F) == 1.25F);
}

TEST_CASE("The luma curve sets the luminance and keeps the channel ratios", "[tonecurve]") {
    DevelopSettings settings;
    settings.toneCurve.luma = sCurve;
    const auto plan = planOf(settings);
    REQUIRE(plan.pointwise.toneCurves.luma.active);
    REQUIRE_FALSE(plan.pointwise.toneCurves.red.active);

    for (const Colour colour : {Colour{0.4F, 0.2F, 0.1F}, Colour{0.05F, 0.3F, 0.9F},
                                Colour{0.02F, 0.01F, 0.005F}, Colour{3.0F, 1.0F, 0.2F}}) {
        CAPTURE(colour[0], colour[1], colour[2]);
        const Colour result = applyToneCurves(plan.pointwise.toneCurves, colour);

        const float expected = toLinear(std::max(
            evaluateCurve(plan.pointwise.toneCurves.luma, toPerceptual(luminanceOf(colour))),
            0.0F));
        REQUIRE(luminanceOf(result) == Approx(expected).epsilon(1.0e-5));
        // Hue and saturation: the channels keep their proportions.
        REQUIRE(result[0] / result[1] == Approx(colour[0] / colour[1]).epsilon(1.0e-5));
        REQUIRE(result[2] / result[1] == Approx(colour[2] / colour[1]).epsilon(1.0e-5));
    }
}

TEST_CASE("A black the luma curve lifts becomes a neutral", "[tonecurve]") {
    DevelopSettings settings;
    settings.toneCurve.luma = curveOf({{0.0F, 0.2F}, {1.0F, 1.0F}});
    const auto plan = planOf(settings);
    const float lifted = toLinear(0.2F);
    for (const Colour black : {Colour{0.0F, 0.0F, 0.0F}, Colour{1.0e-30F, 0.0F, 2.0e-30F}}) {
        const Colour result = applyToneCurves(plan.pointwise.toneCurves, black);
        REQUIRE(result[0] == Approx(lifted));
        REQUIRE(result[1] == Approx(lifted));
        REQUIRE(result[2] == Approx(lifted));
    }
    // A NaN luminance has no ratio at all and takes the lift.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(applyToneCurves(plan.pointwise.toneCurves, {nan, 0.5F, 0.5F}) ==
            Colour{lifted, lifted, lifted});
}

TEST_CASE("A luma curve that lifts black puts every grey on the curve", "[tonecurve]") {
    DevelopSettings settings;
    settings.toneCurve.luma = curveOf({{0.0F, 0.2F}, {0.5F, 0.6F}, {1.0F, 1.0F}});
    const auto plan = planOf(settings);
    for (const float grey : {curveRatioFloor * 1.5F, 0.001F, 0.05F, 0.18F, 0.7F, 1.0F, 3.0F}) {
        CAPTURE(grey);
        const Colour result = applyToneCurves(plan.pointwise.toneCurves, {grey, grey, grey});
        const float expected = toLinear(
            std::max(evaluateCurve(plan.pointwise.toneCurves.luma, toPerceptual(grey)), 0.0F));
        REQUIRE(result[0] == Approx(expected).epsilon(1.0e-5));
        REQUIRE(result[1] == Approx(expected).epsilon(1.0e-5));
        REQUIRE(result[2] == Approx(expected).epsilon(1.0e-5));
    }
}

TEST_CASE("A luma curve that lifts black stays bounded and continuous near zero luminance",
          "[tonecurve]") {
    DevelopSettings settings;
    settings.toneCurve.luma = curveOf({{0.0F, 0.2F}, {1.0F, 1.0F}});
    const auto plan = planOf(settings);
    const float lift = toLinear(0.2F);

    // Out of gamut with next to no luminance: the green that cancels red's
    // luminance is varied through zero luminance.
    for (float green = -0.38720F; green >= -0.38770F; green -= 0.00002F) {
        const Colour colour{1.0F, green, 0.0F};
        CAPTURE(green, luminanceOf(colour));
        const Colour result = applyToneCurves(plan.pointwise.toneCurves, colour);
        // Before the split, the ratio here reached thousands.
        REQUIRE(std::abs(result[0]) < 100.0F);
        REQUIRE(std::abs(result[1]) < 100.0F);
        REQUIRE(result[2] == Approx(lift));
    }
    // Continuous across zero luminance...
    const Colour justAbove = applyToneCurves(plan.pointwise.toneCurves, {1.0F, -0.38745F, 0.0F});
    const Colour justBelow = applyToneCurves(plan.pointwise.toneCurves, {1.0F, -0.38748F, 0.0F});
    REQUIRE(luminanceOf({1.0F, -0.38745F, 0.0F}) > 0.0F);
    REQUIRE(luminanceOf({1.0F, -0.38748F, 0.0F}) < 0.0F);
    REQUIRE(justAbove[0] == Approx(justBelow[0]).epsilon(1.0e-4));
    REQUIRE(justAbove[1] == Approx(justBelow[1]).epsilon(1.0e-3));
    // ...and across the floor.
    const float over = std::nextafter(curveRatioFloor, 1.0F);
    const float under = std::nextafter(curveRatioFloor, 0.0F);
    REQUIRE(applyToneCurves(plan.pointwise.toneCurves, {over, over, over})[0] ==
            Approx(applyToneCurves(plan.pointwise.toneCurves, {under, under, under})[0])
                .epsilon(1.0e-5));

    // At and below the floor the colour is the lift plus a fixed multiple of it.
    const Colour at = applyToneCurves(plan.pointwise.toneCurves,
                                      {curveRatioFloor, curveRatioFloor, curveRatioFloor});
    const float ratio = (at[0] - lift) / curveRatioFloor;
    for (const Colour colour : {Colour{1.0F, -0.38745F, 0.0F}, Colour{-0.5F, -0.5F, -0.5F},
                                Colour{0.0F, 0.0F, 0.0F}, Colour{1.0e-6F, 1.0e-6F, 1.0e-6F}}) {
        CAPTURE(colour[0], colour[1], colour[2]);
        const Colour result = applyToneCurves(plan.pointwise.toneCurves, colour);
        for (std::size_t i = 0; i < 3; ++i) {
            REQUIRE(result[i] == Approx(colour[i] * ratio + lift).margin(1.0e-6));
        }
    }
}

TEST_CASE("A luma curve that keeps black leaves a negative luminance colour its colour",
          "[tonecurve]") {
    // Slope one out of black: near zero luminance the curve is close to the
    // identity, so a colour with negative luminance is kept, not made neutral.
    DevelopSettings settings;
    settings.toneCurve.luma = curveOf({{0.0F, 0.0F}, {0.5F, 0.5F}, {1.0F, 0.8F}});
    const auto plan = planOf(settings);
    const Colour colour{-0.5F, 0.1F, 0.2F};
    REQUIRE(luminanceOf(colour) < 0.0F);
    const Colour result = applyToneCurves(plan.pointwise.toneCurves, colour);
    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(result[i] == Approx(colour[i]).epsilon(0.05));
    }
}

TEST_CASE("The red, green and blue curves act on their own channel only", "[tonecurve]") {
    const Colour colour{0.4F, 0.2F, 0.1F};
    const struct {
        std::size_t channel;
        ToneCurve ToneCurveSettings::* member;
    } cases[] = {{0, &ToneCurveSettings::red},
                 {1, &ToneCurveSettings::green},
                 {2, &ToneCurveSettings::blue}};
    for (const auto& [channel, member] : cases) {
        INFO(channel);
        DevelopSettings settings;
        settings.toneCurve.*member = sCurve;
        const auto plan = planOf(settings);
        const Colour result = applyToneCurves(plan.pointwise.toneCurves, colour);
        const CurvePlan& curve = curvePlanFor(sCurve);
        for (std::size_t i = 0; i < 3; ++i) {
            if (i == channel) {
                REQUIRE(result[i] ==
                        Approx(toLinear(evaluateCurve(curve, toPerceptual(colour[i])))));
                REQUIRE(result[i] != colour[i]);
            } else {
                REQUIRE(result[i] == colour[i]);
            }
        }
    }
}

TEST_CASE("A negative channel moves by the curve's lift", "[tonecurve]") {
    DevelopSettings settings;
    settings.toneCurve.red = curveOf({{0.0F, 0.0F}, {0.5F, 0.7F}, {1.0F, 1.0F}});
    settings.toneCurve.green = curveOf({{0.0F, 0.2F}, {1.0F, 1.0F}});
    const auto plan = planOf(settings);
    const float lift = toLinear(0.2F);

    const Colour result = applyToneCurves(plan.pointwise.toneCurves, {-0.3F, -0.3F, -0.3F});
    // Red's curve starts at zero, so the negative channel passes through;
    // green's lifts black, and the channel moves by the lift.
    REQUIRE(result[0] == -0.3F);
    REQUIRE(result[1] == Approx(-0.3F + lift));
    REQUIRE(result[2] == -0.3F);
    // Continuous where a channel crosses zero, from either side.
    REQUIRE(applyToneCurves(plan.pointwise.toneCurves, {0.0F, 0.0F, 0.0F})[1] == Approx(lift));
    REQUIRE(applyToneCurves(plan.pointwise.toneCurves, {0.0F, -1.0e-12F, 0.0F})[1] == Approx(lift));
    REQUIRE(applyToneCurves(plan.pointwise.toneCurves, {0.0F, 1.0e-12F, 0.0F})[1] ==
            Approx(lift).epsilon(1.0e-3));

    // NaN takes the curve's value at black, as zero does.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Colour fromNan = applyToneCurves(plan.pointwise.toneCurves, {nan, nan, 0.5F});
    REQUIRE(fromNan[0] == 0.0F);
    REQUIRE(fromNan[1] == Approx(lift));
}

TEST_CASE("A curve that touches only the highlights keeps a colour outside the gamut",
          "[tonecurve]") {
    DevelopSettings settings;
    settings.toneCurve.red = curveOf({{0.0F, 0.0F}, {0.9F, 0.91F}, {1.0F, 1.0F}});
    const auto plan = planOf(settings);
    const Colour result = applyToneCurves(plan.pointwise.toneCurves, {-0.1F, 0.5F, 0.5F});
    REQUIRE(result[0] == -0.1F);
    REQUIRE(result[1] == 0.5F);
    REQUIRE(result[2] == 0.5F);
}

TEST_CASE("The curves run after Basic Tone and before the shoulder", "[tonecurve]") {
    DevelopSettings settings;
    settings.tone.exposure = 0.5F;
    settings.tone.contrast = 0.3F;
    settings.tone.filmicHighlights = 0.8F;
    settings.toneCurve.luma = sCurve;
    settings.toneCurve.blue = curveOf({{0.0F, 0.1F}, {0.5F, 0.4F}, {1.0F, 1.0F}});
    const auto plan = planFor(ColorEncoding{workingEncoding}, DevelopState{settings});
    REQUIRE(std::isfinite(plan.pointwise.shoulderKnee));
    REQUIRE(plan.pointwise.tone.shapesTone);

    bool differsFromCurvesLast = false;
    for (const Colour colour : {Colour{0.6F, 0.5F, 0.4F}, Colour{1.5F, 1.0F, 0.2F},
                                Colour{0.05F, 0.04F, 0.03F}, Colour{4.0F, 3.0F, 2.0F}}) {
        CAPTURE(colour[0], colour[1], colour[2]);
        Colour value = plan.pointwise.toWorking * colour;
        value = {value[0] * plan.pointwise.tone.exposureGain,
                 value[1] * plan.pointwise.tone.exposureGain,
                 value[2] * plan.pointwise.tone.exposureGain};
        value = shapeTone(plan.pointwise.tone, value);

        const Colour expected = rollHighlights(plan.pointwise.shoulderKnee,
                                               applyToneCurves(plan.pointwise.toneCurves, value));
        REQUIRE(sameBits(developPixel(plan.pointwise, colour), expected));

        const Colour swapped = applyToneCurves(plan.pointwise.toneCurves,
                                               rollHighlights(plan.pointwise.shoulderKnee, value));
        differsFromCurvesLast = differsFromCurvesLast || !sameBits(expected, swapped);
    }
    REQUIRE(differsFromCurvesLast);
}

TEST_CASE("A curve that lifts past white is caught by the shoulder", "[tonecurve]") {
    DevelopSettings settings;
    settings.toneCurve.luma = curveOf({{0.0F, 0.0F}, {0.8F, 1.0F}, {1.0F, 1.0F}});
    settings.tone.filmicHighlights = 1.0F;
    const auto plan = planFor(ColorEncoding{workingEncoding}, DevelopState{settings});
    const Colour result = developPixel(plan.pointwise, {1.2F, 1.2F, 1.2F});
    REQUIRE(luminanceOf(result) < 1.0F);
}

TEST_CASE("Moving a control point changes the plan", "[tonecurve]") {
    DevelopSettings settings;
    settings.toneCurve.green = sCurve;
    const auto before = planOf(settings);
    REQUIRE(before == planOf(settings));

    settings.toneCurve.green.points[1].y += 0.01F;
    const auto after = planOf(settings);
    REQUIRE_FALSE(before == after);
    REQUIRE(before.pointwise.toneCurves.luma == after.pointwise.toneCurves.luma);
    REQUIRE_FALSE(before.pointwise.toneCurves.green == after.pointwise.toneCurves.green);
    // The curves are pointwise: the pointwise boundary no longer matches.
    REQUIRE_FALSE(prefixMatches(before, after, Stage::Pointwise));

    settings.toneCurve.green = ToneCurve{};
    REQUIRE_FALSE(planOf(settings).pointwise.toneCurves.green.active);
}

TEST_CASE("A curve that is not well formed cannot be planned", "[tonecurve]") {
    CHECK_THROWS_AS(curvePlanFor(curveOf({{0.0F, 0.0F}})), std::invalid_argument);
    CHECK_THROWS_AS(curvePlanFor(curveOf({{0.0F, 0.0F}, {0.5F, 0.5F}, {0.5F, 0.6F}, {1.0F, 1.0F}})),
                    std::invalid_argument);
    CHECK_THROWS_AS(curvePlanFor(curveOf({{0.2F, 0.0F}, {1.0F, 1.0F}})), std::invalid_argument);
    CHECK_THROWS_AS(curvePlanFor(curveOf({{0.0F, 0.0F}, {1.0F, 2.0F}})), std::invalid_argument);
    CHECK_THROWS_AS(
        curvePlanFor(curveOf({{0.0F, 0.0F}, {0.5F, 0.4F}, {0.505F, 0.6F}, {1.0F, 1.0F}})),
        std::invalid_argument);

    DevelopSettings settings;
    settings.toneCurve.red.points = {{0.0F, 0.0F}};
    CHECK_THROWS_AS(planOf(settings), std::invalid_argument);
}

TEST_CASE("Neighbouring points must be at least a hundredth apart in x", "[tonecurve]") {
    REQUIRE(minimumCurvePointSpacing == 0.01F);
    REQUIRE(isWellFormed(curveOf({{0.0F, 0.0F}, {0.01F, 0.5F}, {1.0F, 1.0F}})));
    // 1 - 0.99f is a hair under 0.01f; rounding is not a narrower gap.
    REQUIRE(isWellFormed(curveOf({{0.0F, 0.0F}, {0.99F, 0.5F}, {1.0F, 1.0F}})));
    REQUIRE(isWellFormed(curveOf({{0.0F, 0.0F}, {0.3F, 0.2F}, {0.31F, 0.5F}, {1.0F, 1.0F}})));
    REQUIRE_FALSE(isWellFormed(curveOf({{0.0F, 0.0F}, {0.009F, 0.5F}, {1.0F, 1.0F}})));
    REQUIRE_FALSE(isWellFormed(curveOf({{0.0F, 0.0F}, {0.995F, 0.5F}, {1.0F, 1.0F}})));
    REQUIRE_FALSE(isWellFormed(curveOf({{0.0F, 0.0F}, {0.0005F, 0.5F}, {1.0F, 1.0F}})));
    // Order matters to the invariant; curveFromPoints is what sorts.
    REQUIRE_FALSE(isWellFormed(curveOf({{0.0F, 0.0F}, {1.0F, 1.0F}, {0.5F, 0.5F}})));
}

TEST_CASE("A curve built from points is sorted and has its ends snapped", "[tonecurve]") {
    const auto sorted = curveFromPoints({{1.0F, 1.0F}, {0.3F, 0.7F}, {0.0F, 0.1F}});
    REQUIRE(sorted);
    REQUIRE(sorted->points == std::vector<CurvePoint>{{0.0F, 0.1F}, {0.3F, 0.7F}, {1.0F, 1.0F}});

    const auto snapped = curveFromPoints({{-5.0e-7F, 0.0F}, {0.5F, 0.6F}, {1.0000005F, 1.0F}});
    REQUIRE(snapped);
    REQUIRE(snapped->points.front().x == 0.0F);
    REQUIRE(snapped->points.back().x == 1.0F);

    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    // An end further out than rounding is malformed, not clamped.
    REQUIRE_FALSE(curveFromPoints({{-0.01F, 0.0F}, {1.0F, 1.0F}}));
    REQUIRE_FALSE(curveFromPoints({{0.01F, 0.0F}, {1.0F, 1.0F}}));
    REQUIRE_FALSE(curveFromPoints({{0.0F, 0.0F}, {1.0001F, 1.0F}}));
    REQUIRE_FALSE(curveFromPoints({{0.0F, 0.0F}, {0.5F, 0.2F}, {0.5F, 0.3F}, {1.0F, 1.0F}}));
    REQUIRE_FALSE(curveFromPoints({{0.0F, 0.0F}, {nan, 0.5F}, {1.0F, 1.0F}}));
    REQUIRE_FALSE(curveFromPoints({{0.0F, nan}, {1.0F, 1.0F}}));
    REQUIRE_FALSE(curveFromPoints({{0.0F, 0.0F}}));
    REQUIRE_FALSE(curveFromPoints({}));
    std::vector<CurvePoint> seventeen;
    for (int i = 0; i < 17; ++i) {
        seventeen.push_back({static_cast<float>(i) / 16.0F, 0.5F});
    }
    REQUIRE_FALSE(curveFromPoints(seventeen));
    seventeen.pop_back();
    seventeen.back().x = 1.0F;
    REQUIRE(curveFromPoints(seventeen));
}
