#include "ProcessingPlan.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace arraw;

/// The tone chain of ADR 010: shaping happens in a perceptual coordinate, and
/// one shoulder ends the chain. These test the shapes directly, a colour at a
/// time, because that is where the arithmetic is (ADR 011).

namespace {

/// @brief Returns the luminance the shoulder is judged on.
float luminanceOf(const Colour& colour) {
    return colorspaces::workingLuminance[0] * colour[0] +
           colorspaces::workingLuminance[1] * colour[1] +
           colorspaces::workingLuminance[2] * colour[2];
}

/// @brief Develops one neutral value through a plan's tone chain.
float rolled(float value, float amount) {
    const auto plan = planFor(ColorEncoding{workingEncoding}, {.filmicHighlights = amount});
    return developPixel(plan, {value, value, value})[1];
}

/// @brief Shapes one neutral value, with the shoulder out of the way.
float shaped(float value, float contrast) {
    const auto plan = planFor(ColorEncoding{workingEncoding},
                              {.contrast = contrast, .filmicHighlights = noFilmicHighlights});
    return developPixel(plan, {value, value, value})[1];
}

} // namespace

TEST_CASE("Contrast holds middle grey still", "[tone]") {
    /// What a photographer expects of a contrast control: the grey card does
    /// not move, everything else pivots about it (ADR 013).
    constexpr float grey = 0.18F;

    REQUIRE(std::abs(shaped(grey, 100.0F) - grey) < 1e-6F);
    REQUIRE(std::abs(shaped(grey, -100.0F) - grey) < 1e-6F);
}

TEST_CASE("Contrast pivots about grey in both directions", "[tone]") {
    REQUIRE(shaped(0.05F, 100.0F) < 0.05F);
    REQUIRE(shaped(0.5F, 100.0F) > 0.5F);

    REQUIRE(shaped(0.05F, -100.0F) > 0.05F);
    REQUIRE(shaped(0.5F, -100.0F) < 0.5F);
}

TEST_CASE("Contrast pushes bright values past white, for the shoulder to catch", "[tone]") {
    /// Deliberate: an S that pinned white would flatten the headroom ADR 010
    /// keeps live, which is clipping by another name (ADR 013).
    REQUIRE(shaped(0.9F, 100.0F) > 1.0F);

    /// And with the shoulder in the chain, it comes back below white.
    const auto plan = planFor(ColorEncoding{workingEncoding}, {.contrast = 100.0F});
    REQUIRE(developPixel(plan, {0.9F, 0.9F, 0.9F})[1] < 1.0F);
}

TEST_CASE("Contrast never inverts the tone scale", "[tone]") {
    /// A power law is monotone by construction, and a tone scale that folded
    /// back on itself would render a gradient as a ridge.
    for (const float amount : {-100.0F, -50.0F, 50.0F, 100.0F}) {
        float previous = -1.0F;
        for (int step = 0; step <= 200; ++step) {
            const float value = shaped(static_cast<float>(step) / 100.0F, amount);
            REQUIRE(value > previous);
            previous = value;
        }
    }
}

TEST_CASE("Contrast leaves colour where it was", "[tone]") {
    /// Tone acts on luminance and the colour follows by ratio, so a contrast
    /// control cannot shift a hue (ADR 013).
    const auto plan = planFor(ColorEncoding{workingEncoding},
                              {.contrast = 100.0F, .filmicHighlights = noFilmicHighlights});
    constexpr Colour source{0.3F, 0.2F, 0.1F};

    const Colour developed = developPixel(plan, source);
    const float ratio = developed[0] / source[0];

    REQUIRE(std::abs(developed[1] / source[1] - ratio) < 1e-5F);
    REQUIRE(std::abs(developed[2] / source[2] - ratio) < 1e-5F);
}

TEST_CASE("The plan carries contrast as a slope, not as a slider", "[tone][plan]") {
    const auto plan = planFor(ColorEncoding{workingEncoding}, {.contrast = 200.0F});

    /// Clamped rather than refused, like every other setting (ADR 008).
    REQUIRE(std::abs(plan.contrastSlope - std::sqrt(2.0F)) < 1e-5F);
    REQUIRE(plan.shapesTone);
}

TEST_CASE("A photograph with no tone set is not shaped at all", "[tone][plan]") {
    /// Not merely shaped by an identity: crossing into the perceptual
    /// coordinate and back would return a value a hair from the one it was
    /// given, and 'default settings change nothing' is meant exactly.
    const auto plan = planFor(ColorEncoding{workingEncoding}, {});
    constexpr Colour colour{0.25F, 0.5F, 0.7F};

    REQUIRE_FALSE(plan.shapesTone);
    REQUIRE(plan.contrastSlope == 1.0F);
    REQUIRE(developPixel(plan, colour) == colour);
}

TEST_CASE("The shoulder leaves everything below its knee alone", "[tone]") {
    /// Its whole point: shadows and midtones are not a display transform's
    /// business, so the bend starts where the highlights do.
    REQUIRE(rolled(0.0F, 25.0F) == 0.0F);
    REQUIRE(rolled(0.5F, 25.0F) == 0.5F);
    REQUIRE(rolled(0.875F, 25.0F) == 0.875F);
}

TEST_CASE("The shoulder rolls toward white and never reaches it", "[tone]") {
    /// A hard clip makes every bright value the same white. A shoulder keeps
    /// them apart, however far above white they were.
    const float atWhite = rolled(1.0F, 25.0F);
    const float twoStopsOver = rolled(4.0F, 25.0F);
    const float absurd = rolled(1000.0F, 25.0F);

    REQUIRE(std::abs(atWhite - 0.9375F) < 1e-5F);
    REQUIRE(atWhite < twoStopsOver);
    REQUIRE(twoStopsOver < absurd);
    REQUIRE(absurd < 1.0F);
}

TEST_CASE("The shoulder joins the straight part smoothly", "[tone]") {
    /// A kink at the knee would show as a visible edge in a gradient, so the
    /// bend starts at slope one rather than turning a corner.
    constexpr float step = 1e-4F;
    const float knee = 0.875F;
    const float slope = (rolled(knee + step, 25.0F) - rolled(knee, 25.0F)) / step;

    REQUIRE(std::abs(slope - 1.0F) < 1e-2F);
}

TEST_CASE("A stronger amount starts the roll earlier", "[tone]") {
    /// The amount is where the bend begins: gentle catches only what would
    /// have clipped, strong reaches down into the upper midtones.
    REQUIRE(rolled(0.7F, 25.0F) == 0.7F);
    REQUIRE(rolled(0.7F, 100.0F) < 0.7F);
    REQUIRE(rolled(2.0F, 100.0F) < rolled(2.0F, 25.0F));
}

TEST_CASE("No roll-off at all is allowed, and clips", "[tone]") {
    /// Zero is a true neutral rather than a floor: a photographer who wants a
    /// hard clip may have one, and the number means what it says (ADR 010).
    const auto plan = planFor(ColorEncoding{workingEncoding}, {.filmicHighlights = 0.0F});

    REQUIRE(std::isinf(plan.shoulderKnee));
    REQUIRE(rolled(4.0F, 0.0F) == 4.0F);
}

TEST_CASE("A rolled highlight loses its colour on the way to white", "[tone]") {
    /// Real overexposure desaturates; a highlight that keeps its colour while
    /// being compressed reads as coloured plastic rather than as light.
    const auto plan = planFor(ColorEncoding{workingEncoding}, {});
    constexpr Colour bright{4.0F, 0.5F, 0.1F};

    const Colour developed = developPixel(plan, bright);
    const float ratio = luminanceOf(developed) / luminanceOf(bright);
    const Colour scaledOnly{bright[0] * ratio, bright[1] * ratio, bright[2] * ratio};

    /// Nearer its own luminance than a plain scaling would leave it.
    const float rolledSpread = developed[0] - developed[2];
    const float scaledSpread = scaledOnly[0] - scaledOnly[2];
    REQUIRE(rolledSpread < scaledSpread);

    /// The luminance the shoulder promised is the one that comes out: the
    /// fade toward white moves colour, not brightness.
    REQUIRE(std::abs(luminanceOf(developed) - luminanceOf(scaledOnly)) < 1e-5F);
}

TEST_CASE("A neutral highlight stays neutral", "[tone]") {
    const auto plan = planFor(ColorEncoding{workingEncoding}, {});
    const Colour developed = developPixel(plan, {3.0F, 3.0F, 3.0F});

    REQUIRE(developed[0] == developed[1]);
    REQUIRE(developed[1] == developed[2]);
}

TEST_CASE("The plan carries the knee, not the amount", "[tone][plan]") {
    /// Settings are what a photographer sets; a plan is what the pixels need,
    /// and what the pixels need is where the bend starts (ADR 011).
    REQUIRE(planFor(ColorEncoding{workingEncoding}, {.filmicHighlights = 100.0F}).shoulderKnee ==
            0.5F);
    REQUIRE(planFor(ColorEncoding{workingEncoding}, {.filmicHighlights = 25.0F}).shoulderKnee ==
            0.875F);

    /// Out of range is clamped rather than refused, like every other setting:
    /// no pixel maths depends on a caller having checked first (ADR 008).
    REQUIRE(planFor(ColorEncoding{workingEncoding}, {.filmicHighlights = 400.0F}).shoulderKnee ==
            0.5F);
    REQUIRE(std::isinf(
        planFor(ColorEncoding{workingEncoding}, {.filmicHighlights = -10.0F}).shoulderKnee));
}
