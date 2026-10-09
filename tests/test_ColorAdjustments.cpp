#include "ColorAdjustments.h"
#include "ProcessingPlan.h"

#include <DevelopSettings.h>
#include <DevelopState.h>
#include <SettingsJson.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;
using Catch::Approx;

/// The colour block of ADR 027: saturation, vibrance, HSL and Black & White,
/// run after the shoulder. The maths is a port of main's, so these test its
/// behaviour a colour at a time rather than its constants.

namespace {

/// @brief Resolves settings into a plan, with the shoulder out of the way.
ProcessingPlan planOf(DevelopSettings settings) {
    settings.tone.filmicHighlights = noFilmicHighlights;
    return planFor(ColorEncoding{workingEncoding}, DevelopState{settings});
}

/// @brief Develops one colour through settings, with the shoulder out of the way.
Colour developed(const DevelopSettings& settings, Colour colour) {
    return developPixel(planOf(settings).pointwise, colour);
}

/// @brief Oklab chroma of a colour.
float chromaOf(Colour colour) {
    const Oklab lab = toOklab(colour);
    return std::sqrt(lab.a * lab.a + lab.b * lab.b);
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

/// @brief Colours across the awkward places: black, neutral, vivid, above white, negative, NaN.
std::vector<Colour> awkwardColours() {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    return {{0.0F, 0.0F, 0.0F}, {0.18F, 0.18F, 0.18F}, {1.0F, 1.0F, 1.0F},
            {1.0F, 0.0F, 0.0F}, {0.2F, 0.6F, 0.1F},    {0.05F, 0.3F, 0.9F},
            {4.0F, 2.0F, 0.5F}, {-0.1F, 0.4F, 0.2F},   {1.0e-9F, 0.0F, 3.0e-9F},
            {nan, 0.5F, 0.5F},  {inf, 0.0F, 0.0F},     {0.25F, 0.5F, 0.7F}};
}

constexpr Colour pureRed{0.8F, 0.0F, 0.0F};
constexpr Colour pureGreen{0.0F, 0.7F, 0.0F};
constexpr Colour muted{0.45F, 0.40F, 0.38F};
constexpr Colour vivid{0.85F, 0.10F, 0.05F};

} // namespace

TEST_CASE("Default settings leave every colour bit for bit alone", "[colour]") {
    const auto plan = planFor(ColorEncoding{workingEncoding}, DevelopState{});
    REQUIRE(plan.pointwise.colorAdjustments == ColorAdjustmentPlan{});
    REQUIRE_FALSE(plan.pointwise.colorAdjustments.chroma.adjustsSaturation);
    REQUIRE_FALSE(plan.pointwise.colorAdjustments.chroma.adjustsVibrance);
    REQUIRE_FALSE(plan.pointwise.colorAdjustments.adjustsHsl);
    REQUIRE_FALSE(plan.pointwise.colorAdjustments.convertsToGrayscale);

    for (const Colour colour : awkwardColours()) {
        CAPTURE(colour[0], colour[1], colour[2]);
        REQUIRE(sameBits(adjustColor(plan.pointwise.colorAdjustments, colour), colour));
        // The same chain as before the colour block existed: matrix, gain, tone
        // (off) and the default shoulder, with nothing after it.
        ProcessingPlan without = plan;
        without.pointwise.colorAdjustments = {};
        REQUIRE(sameBits(developPixel(plan.pointwise, colour),
                         developPixel(without.pointwise, colour)));
    }
}

TEST_CASE("Each colour control resolves to its own flag", "[colour][plan]") {
    const auto resolve = [](DevelopSettings settings) {
        return planFor(ColorEncoding{workingEncoding}, DevelopState{settings})
            .pointwise.colorAdjustments;
    };

    DevelopSettings settings;
    settings.color.saturation = 50.0F;
    auto block = resolve(settings);
    REQUIRE(block.chroma.adjustsSaturation);
    REQUIRE(block.chroma.saturation == 0.5F);
    REQUIRE_FALSE(block.chroma.adjustsVibrance);
    REQUIRE_FALSE(block.adjustsHsl);

    settings = {};
    settings.color.vibrance = -100.0F;
    block = resolve(settings);
    REQUIRE(block.chroma.adjustsVibrance);
    REQUIRE(block.chroma.vibrance == -1.0F);

    settings = {};
    settings.hsl.aqua.luminance = 20.0F;
    block = resolve(settings);
    REQUIRE(block.adjustsHsl);
    REQUIRE(block.bandLuminance[4] == 0.2F);
    REQUIRE(block.hueShift[4] == 0.0F);

    settings = {};
    settings.blackAndWhite.convertToGrayscale = true;
    settings.blackAndWhite.blue = -40.0F;
    block = resolve(settings);
    REQUIRE(block.convertsToGrayscale);
    REQUIRE(block.grayMix[5] == -40.0F);

    SECTION("out of range is clamped, not refused") {
        settings = {};
        settings.color.saturation = 400.0F;
        settings.hsl.red.hue = -250.0F;
        settings.blackAndWhite.magenta = 999.0F;
        block = resolve(settings);
        REQUIRE(block.chroma.saturation == 1.0F);
        REQUIRE(block.hueShift[0] == -1.0F);
        REQUIRE(block.grayMix[7] == 100.0F);
    }
    SECTION("a value that is not finite is refused") {
        settings = {};
        settings.hsl.green.saturation = std::numeric_limits<float>::quiet_NaN();
        REQUIRE_THROWS_AS(resolve(settings), std::invalid_argument);
        settings = {};
        settings.color.vibrance = std::numeric_limits<float>::infinity();
        REQUIRE_THROWS_AS(resolve(settings), std::invalid_argument);
    }
}

TEST_CASE("The colour settings belong to the pointwise group of the plan", "[colour][plan]") {
    const auto planWith = [](auto change) {
        DevelopSettings settings;
        change(settings);
        return planOf(settings);
    };
    const ProcessingPlan base = planWith([](DevelopSettings&) {});
    const std::vector<ProcessingPlan> changed{
        planWith([](DevelopSettings& s) { s.color.saturation = 10.0F; }),
        planWith([](DevelopSettings& s) { s.color.vibrance = 10.0F; }),
        planWith([](DevelopSettings& s) { s.hsl.blue.hue = 10.0F; }),
        planWith([](DevelopSettings& s) { s.blackAndWhite.convertToGrayscale = true; }),
        planWith([](DevelopSettings& s) { s.blackAndWhite.red = 10.0F; }),
    };
    for (const ProcessingPlan& plan : changed) {
        REQUIRE(plan != base);
        REQUIRE(std::get<0>(stagesOf(plan)) == std::get<0>(stagesOf(base)));
        REQUIRE(std::get<1>(stagesOf(plan)) != std::get<1>(stagesOf(base)));
        REQUIRE(std::get<2>(stagesOf(plan)) == std::get<2>(stagesOf(base)));
        REQUIRE(std::get<3>(stagesOf(plan)) == std::get<3>(stagesOf(base)));
        REQUIRE_FALSE(prefixMatches(plan, base, Stage::Pointwise));
        REQUIRE_FALSE(prefixMatches(plan, base, Stage::Resize));
    }
}

TEST_CASE("Saturation minus a hundred is grey and plus a hundred doubles the chroma",
          "[colour][saturation]") {
    const Colour colour{0.5F, 0.25F, 0.2F};
    DevelopSettings settings;

    settings.color.saturation = weakestSaturation;
    REQUIRE(chromaOf(developed(settings, colour)) == Approx(0.0F).margin(1e-4F));

    settings.color.saturation = strongestSaturation;
    const Colour doubled = developed(settings, colour);
    REQUIRE(chromaOf(doubled) == Approx(2.0F * chromaOf(colour)).epsilon(1e-3));
    // Chroma moves; lightness does not.
    REQUIRE(toOklab(doubled).lightness == Approx(toOklab(colour).lightness).margin(1e-4F));
}

TEST_CASE("Saturation leaves a neutral neutral", "[colour][saturation]") {
    DevelopSettings settings;
    settings.color.saturation = 80.0F;
    const Colour out = developed(settings, {0.4F, 0.4F, 0.4F});
    REQUIRE(out[0] == Approx(out[1]).margin(1e-5F));
    REQUIRE(out[1] == Approx(out[2]).margin(1e-5F));
}

TEST_CASE("Vibrance moves muted colours more than vivid ones", "[colour][vibrance]") {
    DevelopSettings settings;
    settings.color.vibrance = 50.0F;
    const float mutedGain = chromaOf(developed(settings, muted)) / chromaOf(muted);
    const float vividGain = chromaOf(developed(settings, vivid)) / chromaOf(vivid);

    REQUIRE(mutedGain > vividGain);
    REQUIRE(vividGain > 1.0F);
}

TEST_CASE("An HSL hue shift moves its band and leaves the others be", "[colour][hsl]") {
    DevelopSettings settings;
    settings.hsl.red.hue = strongestHslControl;

    const Colour turned = developed(settings, pureRed);
    // Red turns toward orange, keeping its value. Not by the whole thirty
    // degrees: the neighbouring bands weigh in too, with no shift of their own,
    // and the weights are normalised.
    REQUIRE(turned[0] == Approx(0.8F).margin(1e-5F));
    REQUIRE(turned[1] > 0.15F);
    REQUIRE(turned[1] < 0.4F);
    REQUIRE(turned[2] == Approx(0.0F).margin(1e-5F));

    const Colour untouched = developed(settings, pureGreen);
    REQUIRE(untouched[0] == Approx(pureGreen[0]).margin(1e-5F));
    REQUIRE(untouched[1] == Approx(pureGreen[1]).margin(1e-5F));
    REQUIRE(untouched[2] == Approx(pureGreen[2]).margin(1e-5F));
}

TEST_CASE("HSL saturation and luminance act on their band", "[colour][hsl]") {
    DevelopSettings settings;
    settings.hsl.green.saturation = -100.0F;
    const Colour grey = developed(settings, {0.2F, 0.6F, 0.2F});
    // Half the saturation, as a hundred is fifty percent of it.
    REQUIRE(grey[0] == Approx(0.4F).margin(1e-4F));
    REQUIRE(grey[1] == Approx(0.6F).margin(1e-5F));

    settings = {};
    settings.hsl.green.luminance = 100.0F;
    const Colour lighter = developed(settings, pureGreen);
    REQUIRE(lighter[1] == Approx(1.2F).margin(1e-5F)); // not clamped to white

    settings.hsl.green.luminance = -100.0F;
    REQUIRE(developed(settings, pureGreen)[1] == Approx(0.2F).margin(1e-5F));

    // A black pixel has no hue to select a band by.
    REQUIRE(developed(settings, {0.0F, 0.0F, 0.0F}) == Colour{0.0F, 0.0F, 0.0F});
}

TEST_CASE("HSL band weights blend by distance and normalise", "[colour][hsl]") {
    // Halfway between the red and orange centres (0 and 0.083 of the wheel),
    // both bands weigh in equally.
    constexpr Colour between{1.0F, 0.249F, 0.0F};
    DevelopSettings settings;

    settings.hsl.red.hue = 100.0F;
    const Colour half = developed(settings, between);
    REQUIRE(half[0] == Approx(1.0F).margin(1e-5F));
    REQUIRE(half[1] > 0.249F + 0.05F); // toward orange and yellow

    settings.hsl.orange.hue = -100.0F;
    const Colour cancelled = developed(settings, between);
    REQUIRE(cancelled[1] == Approx(0.249F).margin(2e-3F));

    // Hue wraps around the wheel: magenta turns a hue just short of red across zero.
    settings = {};
    settings.hsl.magenta.hue = 100.0F;
    const Colour wrapped = developed(settings, {1.0F, 0.0F, 0.1F});
    REQUIRE(wrapped[0] == Approx(1.0F).margin(1e-5F));
    // Past red and out the other side, into orange: blue is gone, green grew.
    REQUIRE(wrapped[1] > 0.0F);
    REQUIRE(wrapped[2] == Approx(0.0F).margin(1e-5F));
}

TEST_CASE("Black and white makes a grey that is the luminance when the mix is flat",
          "[colour][blackandwhite]") {
    DevelopSettings settings;
    settings.blackAndWhite.convertToGrayscale = true;
    for (const Colour colour : {Colour{0.8F, 0.1F, 0.1F}, Colour{0.1F, 0.5F, 0.9F}, muted}) {
        const Colour out = developed(settings, colour);
        const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                                colorspaces::workingLuminance[1] * colour[1] +
                                colorspaces::workingLuminance[2] * colour[2];
        REQUIRE(out[0] == out[1]);
        REQUIRE(out[1] == out[2]);
        REQUIRE(out[0] == Approx(luminance).epsilon(1e-5));
    }
}

TEST_CASE("The black and white mixer never moves a neutral", "[colour][blackandwhite]") {
    DevelopSettings settings;
    settings.blackAndWhite = {true, 100.0F, -100.0F, 80.0F, -60.0F, 40.0F, -20.0F, 90.0F, -90.0F};
    for (const float value : {0.02F, 0.18F, 0.5F, 1.0F}) {
        const Colour out = developed(settings, {value, value, value});
        REQUIRE(out[0] == Approx(value).epsilon(1e-5));
    }
}

TEST_CASE("A band's weight brightens or darkens its hue in the grey", "[colour][blackandwhite]") {
    DevelopSettings settings;
    settings.blackAndWhite.convertToGrayscale = true;
    const float flat = developed(settings, pureRed)[0];

    settings.blackAndWhite.red = 60.0F;
    const Colour brighter = developed(settings, pureRed);
    REQUIRE(brighter[0] > flat);
    // Less than the whole sixty percent: the neighbouring bands, with weights
    // of their own at zero, take their share of a pure red's blend.
    REQUIRE(brighter[0] < flat * 1.6F);
    REQUIRE(brighter[1] == brighter[2]);

    settings.blackAndWhite.red = -100.0F;
    const float darker = developed(settings, pureRed)[0];
    REQUIRE(darker < flat);
    REQUIRE(darker >= 0.0F);

    // The green is not red's business.
    settings.blackAndWhite.red = 100.0F;
    settings.blackAndWhite.convertToGrayscale = true;
    const float greenGrey = developed(settings, pureGreen)[0];
    settings.blackAndWhite.red = 0.0F;
    REQUIRE(developed(settings, pureGreen)[0] == Approx(greenGrey).epsilon(1e-6));
}

TEST_CASE("Black and white replaces the colour controls", "[colour][blackandwhite]") {
    DevelopSettings plain;
    plain.blackAndWhite.convertToGrayscale = true;
    plain.blackAndWhite.green = 30.0F;

    DevelopSettings busy = plain;
    busy.color.saturation = 100.0F;
    busy.color.vibrance = -50.0F;
    busy.hsl.red.hue = 100.0F;
    busy.hsl.green.luminance = 60.0F;

    for (const Colour colour : awkwardColours()) {
        CAPTURE(colour[0], colour[1], colour[2]);
        REQUIRE(sameBits(developed(busy, colour), developed(plain, colour)));
    }
}

TEST_CASE("The colour block runs after the shoulder", "[colour]") {
    // Far above white and saturated: the shoulder rolls it, then saturation
    // acts on what the shoulder left, so the result is the shoulder's output
    // with its chroma scaled and not the other way round.
    DevelopSettings settings;
    settings.tone.filmicHighlights = 50.0F;
    settings.color.saturation = weakestSaturation;
    const auto plan = planFor(ColorEncoding{workingEncoding}, DevelopState{settings});
    const Colour colour{6.0F, 2.0F, 0.5F};

    const Colour rolled =
        rollHighlights(plan.pointwise.shoulderKnee,
                       shapeTone(plan.pointwise.tone, plan.pointwise.toWorking * colour));
    const Colour out = developPixel(plan.pointwise, colour);
    REQUIRE(chromaOf(out) == Approx(0.0F).margin(1e-4F));
    REQUIRE(toOklab(out).lightness == Approx(toOklab(rolled).lightness).margin(1e-4F));
}

TEST_CASE("Colour settings survive a JSON round trip", "[colour][json]") {
    DevelopSettings settings;
    settings.color.saturation = -37.5F;
    settings.color.vibrance = 12.25F;
    settings.hsl.purple = {.hue = 20.5F, .saturation = -30.0F, .luminance = 45.75F};
    settings.hsl.red.hue = -100.0F;
    settings.blackAndWhite = {true, 10.0F, 20.0F, 30.0F, 40.0F, 50.0F, 60.0F, 70.0F, -80.0F};

    const std::string text = settingsToJson(settings);
    REQUIRE(text.find("\"saturation\": -37.5") != std::string::npos);
    REQUIRE(text.find("\"luminancePurple\": 45.75") != std::string::npos);
    REQUIRE(text.find("\"convertToGrayscale\": true") != std::string::npos);
    REQUIRE(applySettingsJson(text, DevelopSettings{}) == settings);
}
