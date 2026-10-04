#include "ColorAdjustments.h"
#include "ColorGrading.h"
#include "ColorSpaces.h"
#include "ProcessingPlan.h"

#include <DevelopSettings.h>
#include <DevelopState.h>
#include <SettingDescriptors.h>
#include <SettingsJson.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;
using Catch::Approx;

/// Colour Grading: three zones of hue and saturation weighed on the
/// perceptual coordinate, Balance and Blending, a tint in Oklab at the end of
/// the colour block. A port of main's ADR 0052, so these test its behaviour
/// rather than its constants.

namespace {

/// @brief Resolves settings into a plan, with the shoulder out of the way.
ProcessingPlan planOf(DevelopSettings settings) {
    settings.tone.filmicHighlights = noFilmicHighlights;
    return planFor(ColorEncoding{workingEncoding}, DevelopState{settings});
}

/// @brief Develops one colour through settings, with the shoulder out of the way.
Colour developed(const DevelopSettings& settings, Colour colour) {
    return developPixel(planOf(settings), colour);
}

/// @brief Builds settings with a grade and nothing else.
DevelopSettings graded(const ColorGradingSettings& grading) {
    DevelopSettings settings;
    settings.colorGrading = grading;
    return settings;
}

/// @brief Oklab chroma of a colour.
float chromaOf(Colour colour) {
    const Oklab lab = toOklab(colour);
    return std::hypot(lab.a, lab.b);
}

/// @brief Oklab hue of a colour, in degrees from zero up to 360.
float hueOf(Colour colour) {
    const Oklab lab = toOklab(colour);
    const float degrees = std::atan2(lab.b, lab.a) * 180.0F / std::numbers::pi_v<float>;
    return degrees < 0.0F ? degrees + 360.0F : degrees;
}

/// @brief Distance between two hues around the wheel, in degrees.
float hueDistance(float first, float second) {
    const float d = std::fmod(std::abs(first - second), 360.0F);
    return d > 180.0F ? 360.0F - d : d;
}

/// @brief A neutral grey of a linear luminance.
Colour grey(float luminance) {
    return {luminance, luminance, luminance};
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

/// @brief Greys from deep shadow to white, in linear luminance.
constexpr std::array<float, 7> greyRamp{0.002F, 0.02F, 0.08F, 0.18F, 0.4F, 0.7F, 1.0F};

/// @brief A grade tinting only the Shadows zone, toward blue.
ColorGradingSettings blueShadows(float balance = 0.0F, float blending = 50.0F) {
    return {.shadows = {.hue = 260.0F, .saturation = strongestGrade},
            .balance = balance,
            .blending = blending};
}

} // namespace

TEST_CASE("A grade with no saturation leaves every colour bit for bit alone", "[grading]") {
    const ColorGradingSettings hueOnly{.shadows = {.hue = 30.0F},
                                       .midtones = {.hue = 140.0F},
                                       .highlights = {.hue = 300.0F},
                                       .balance = -70.0F,
                                       .blending = 5.0F};
    for (const ColorGradingSettings& grading : {ColorGradingSettings{}, hueOnly}) {
        const ProcessingPlan plan =
            planFor(ColorEncoding{workingEncoding}, DevelopState{graded(grading)});
        // Off, and the plan the defaults make: Balance and Blending alone
        // change nothing, not even what a checkpoint compares.
        REQUIRE_FALSE(plan.colorAdjustments.grading.active);
        REQUIRE(plan.colorAdjustments.grading == ColorGradingPlan{});
        REQUIRE(plan == planFor(ColorEncoding{workingEncoding}, DevelopState{}));
        for (const Colour colour : awkwardColours()) {
            CAPTURE(colour[0], colour[1], colour[2]);
            REQUIRE(sameBits(applyColorGrading(plan.colorAdjustments.grading, colour), colour));
            REQUIRE(sameBits(adjustColor(plan.colorAdjustments, colour), colour));
        }
    }
}

TEST_CASE("A negative saturation clamps to none and leaves the grade off", "[grading][plan]") {
    const ColorGradingSettings negative{.shadows = {.hue = 30.0F, .saturation = -5.0F},
                                        .midtones = {.hue = 140.0F, .saturation = -100.0F},
                                        .highlights = {.hue = 300.0F, .saturation = -1.0e-3F},
                                        .balance = 40.0F,
                                        .blending = 10.0F};
    const ProcessingPlan plan =
        planFor(ColorEncoding{workingEncoding}, DevelopState{graded(negative)});
    REQUIRE_FALSE(plan.colorAdjustments.grading.active);
    REQUIRE(plan.colorAdjustments.grading == ColorGradingPlan{});
    REQUIRE(plan == planFor(ColorEncoding{workingEncoding}, DevelopState{}));
    for (const Colour colour : awkwardColours()) {
        CAPTURE(colour[0], colour[1], colour[2]);
        REQUIRE(sameBits(adjustColor(plan.colorAdjustments, colour), colour));
    }
}

TEST_CASE("A grade resolves to zone tints, a balance shift and a zone width", "[grading][plan]") {
    const auto resolve = [](const ColorGradingSettings& grading) {
        return colorGradingPlanFor(grading);
    };

    const ColorGradingPlan plan = resolve({.midtones = {.hue = 90.0F, .saturation = 50.0F}});
    REQUIRE(plan.active);
    // Half saturation at ninety degrees: straight along +b.
    REQUIRE(plan.midtoneTint.a == Approx(0.0F).margin(1e-7F));
    REQUIRE(plan.midtoneTint.b > 0.0F);
    REQUIRE(plan.shadowTint == ZoneTint{});
    REQUIRE(plan.highlightTint == ZoneTint{});
    REQUIRE(plan.balanceShift == 0.0F);

    // Full saturation is twice half, whatever the direction.
    const ColorGradingPlan full = resolve({.highlights = {.hue = 200.0F, .saturation = 100.0F}});
    REQUIRE(std::hypot(full.highlightTint.a, full.highlightTint.b) ==
            Approx(2.0F * plan.midtoneTint.b).epsilon(1e-6));

    // 0 and 360 are the same hue.
    REQUIRE(resolve({.shadows = {.hue = 0.0F, .saturation = 40.0F}}).shadowTint.a ==
            Approx(resolve({.shadows = {.hue = 360.0F, .saturation = 40.0F}}).shadowTint.a));

    // Balance shifts the position by its sign; Blending only widens.
    const ColorGradingPlan left = resolve(blueShadows(-100.0F, 0.0F));
    const ColorGradingPlan right = resolve(blueShadows(100.0F, 100.0F));
    REQUIRE(left.balanceShift < 0.0F);
    REQUIRE(right.balanceShift == -left.balanceShift);
    REQUIRE(right.zoneWidth > left.zoneWidth);

    SECTION("out of range is clamped, not refused") {
        const ColorGradingPlan clamped = resolve({.shadows = {.hue = 40.0F, .saturation = 300.0F},
                                                  .balance = -900.0F,
                                                  .blending = 500.0F});
        const ColorGradingPlan limits = resolve({.shadows = {.hue = 40.0F, .saturation = 100.0F},
                                                 .balance = -100.0F,
                                                 .blending = 100.0F});
        REQUIRE(clamped == limits);
    }
    SECTION("a hue out of range wraps onto the wheel") {
        const auto tintAt = [&](float hue) {
            return resolve({.shadows = {.hue = hue, .saturation = 60.0F}}).shadowTint;
        };
        REQUIRE(tintAt(400.0F) == tintAt(40.0F));
        REQUIRE(tintAt(-30.0F) == tintAt(330.0F));
        REQUIRE(tintAt(720.0F) == tintAt(0.0F));
        REQUIRE(tintAt(360.0F) == tintAt(0.0F));
        REQUIRE(tintAt(-1080.5F) == tintAt(359.5F));
        // Small enough to round to a full turn once wrapped: still zero.
        REQUIRE(tintAt(-1.0e-6F) == tintAt(0.0F));
    }
    SECTION("a value that is not finite is refused") {
        constexpr float nan = std::numeric_limits<float>::quiet_NaN();
        constexpr float inf = std::numeric_limits<float>::infinity();
        REQUIRE_THROWS_AS(resolve({.shadows = {.hue = nan}}), std::invalid_argument);
        REQUIRE_THROWS_AS(resolve({.midtones = {.saturation = inf}}), std::invalid_argument);
        // Refused even when nothing is tinted: a NaN is never a setting.
        REQUIRE_THROWS_AS(resolve({.balance = nan}), std::invalid_argument);
        REQUIRE_THROWS_AS(resolve({.blending = -inf}), std::invalid_argument);
    }
}

TEST_CASE("The grade belongs to the pointwise group of the plan", "[grading][plan]") {
    const ProcessingPlan base = planOf({});
    const ProcessingPlan plan = planOf(graded({.highlights = {.hue = 50.0F, .saturation = 10.0F}}));
    REQUIRE(plan != base);
    REQUIRE(std::get<0>(stagesOf(plan)) != std::get<0>(stagesOf(base)));
    REQUIRE(std::get<1>(stagesOf(plan)) == std::get<1>(stagesOf(base)));
    REQUIRE(std::get<2>(stagesOf(plan)) == std::get<2>(stagesOf(base)));
    REQUIRE_FALSE(prefixMatches(plan, base, Stage::Pointwise));
}

TEST_CASE("A neutral grey takes on the hue of the zone it falls in", "[grading]") {
    struct Case {
        const char* name;
        ColorGradingSettings grading;
        float luminance;
        float hue;
    };
    const std::array<Case, 4> cases{{
        {"shadows", {.shadows = {.hue = 250.0F, .saturation = 80.0F}}, 0.01F, 250.0F},
        {"midtones", {.midtones = {.hue = 140.0F, .saturation = 80.0F}}, 0.18F, 140.0F},
        {"highlights", {.highlights = {.hue = 60.0F, .saturation = 80.0F}}, 0.8F, 60.0F},
        {"wrapping red", {.midtones = {.hue = 350.0F, .saturation = 80.0F}}, 0.18F, 350.0F},
    }};
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const Colour out = developed(graded(c.grading), grey(c.luminance));
            REQUIRE(chromaOf(out) > 0.02F);
            REQUIRE(hueDistance(hueOf(out), c.hue) < 1.0F);
        }
    }

    // Each zone tints its own greys most.
    const DevelopSettings shadowsOnly = graded({.shadows = {.hue = 250.0F, .saturation = 80.0F}});
    const DevelopSettings highlightsOnly =
        graded({.highlights = {.hue = 60.0F, .saturation = 80.0F}});
    REQUIRE(chromaOf(developed(shadowsOnly, grey(0.01F))) >
            chromaOf(developed(shadowsOnly, grey(0.8F))));
    REQUIRE(chromaOf(developed(highlightsOnly, grey(0.8F))) >
            chromaOf(developed(highlightsOnly, grey(0.01F))));
}

TEST_CASE("Grading holds Oklab lightness", "[grading]") {
    const DevelopSettings settings = graded({.shadows = {.hue = 230.0F, .saturation = 100.0F},
                                             .midtones = {.hue = 20.0F, .saturation = 60.0F},
                                             .highlights = {.hue = 90.0F, .saturation = 100.0F},
                                             .balance = 30.0F,
                                             .blending = 70.0F});
    std::vector<Colour> colours{{0.8F, 0.1F, 0.05F},
                                {0.1F, 0.5F, 0.2F},
                                {0.3F, 0.3F, 0.9F},
                                {0.45F, 0.4F, 0.38F},
                                {2.0F, 1.5F, 1.0F}};
    for (const float luminance : greyRamp) {
        colours.push_back(grey(luminance));
    }
    for (const Colour colour : colours) {
        CAPTURE(colour[0], colour[1], colour[2]);
        const Colour out = developed(settings, colour);
        // At and above white the tint has faded out (see below).
        if (toOklab(colour).lightness < 0.99F) {
            REQUIRE(chromaOf(out) != Approx(chromaOf(colour)).margin(1e-3F));
        }
        REQUIRE(toOklab(out).lightness == Approx(toOklab(colour).lightness).margin(2e-5F));
    }
}

TEST_CASE("Negative Balance hands more of the range to the Shadows zone", "[grading][balance]") {
    // A midtone grey: how much of the shadows' tint it takes is how far the
    // Shadows zone reaches.
    const auto shadowTintAt = [](float balance, float luminance) {
        return chromaOf(developed(graded(blueShadows(balance)), grey(luminance)));
    };
    for (const float luminance : {0.05F, 0.18F, 0.4F}) {
        CAPTURE(luminance);
        REQUIRE(shadowTintAt(-100.0F, luminance) > shadowTintAt(0.0F, luminance));
        REQUIRE(shadowTintAt(0.0F, luminance) > shadowTintAt(100.0F, luminance));
    }

    // In the weights themselves, and the Highlights zone the other way round.
    const ColorGradingPlan toShadows = colorGradingPlanFor(blueShadows(-50.0F));
    const ColorGradingPlan even = colorGradingPlanFor(blueShadows(0.0F));
    const ZoneWeights left = gradeZoneWeights(toShadows, 0.18F);
    const ZoneWeights middle = gradeZoneWeights(even, 0.18F);
    REQUIRE(left.shadows > middle.shadows);
    REQUIRE(left.highlights < middle.highlights);
}

TEST_CASE("Blending widens the transitions between the zones", "[grading][blending]") {
    const ColorGradingPlan sharp = colorGradingPlanFor(blueShadows(0.0F, sharpestGradeBlending));
    const ColorGradingPlan soft = colorGradingPlanFor(blueShadows(0.0F, softestGradeBlending));

    // At the centre of each zone it holds less of the weight when blended...
    REQUIRE(gradeZoneWeights(soft, 0.0F).shadows < gradeZoneWeights(sharp, 0.0F).shadows);
    REQUIRE(gradeZoneWeights(soft, toLinear(0.5F)).midtones <
            gradeZoneWeights(sharp, toLinear(0.5F)).midtones);
    REQUIRE(gradeZoneWeights(soft, 1.0F).highlights < gradeZoneWeights(sharp, 1.0F).highlights);
    // ...and reaches further into its neighbour's.
    REQUIRE(gradeZoneWeights(soft, toLinear(0.5F)).shadows >
            gradeZoneWeights(sharp, toLinear(0.5F)).shadows);
    REQUIRE(gradeZoneWeights(soft, toLinear(0.9F)).shadows >
            gradeZoneWeights(sharp, toLinear(0.9F)).shadows);
}

TEST_CASE("The zone weights sum to one and are held to black and white", "[grading]") {
    const ColorGradingPlan plan = colorGradingPlanFor(blueShadows(40.0F, 20.0F));
    for (const float luminance : {-1.0F, 0.0F, 0.001F, 0.18F, 0.5F, 1.0F, 8.0F}) {
        CAPTURE(luminance);
        const ZoneWeights w = gradeZoneWeights(plan, luminance);
        REQUIRE(w.shadows + w.midtones + w.highlights == Approx(1.0F).epsilon(1e-6));
        REQUIRE(w.shadows >= 0.0F);
        REQUIRE(w.midtones >= 0.0F);
        REQUIRE(w.highlights >= 0.0F);
    }
    const ZoneWeights black = gradeZoneWeights(plan, 0.0F);
    const ZoneWeights below = gradeZoneWeights(plan, -1.0F);
    const ZoneWeights white = gradeZoneWeights(plan, 1.0F);
    const ZoneWeights above = gradeZoneWeights(plan, 8.0F);
    REQUIRE(below.shadows == black.shadows);
    REQUIRE(above.highlights == white.highlights);
}

TEST_CASE("Grading tints a black and white photograph", "[grading][blackandwhite]") {
    DevelopSettings mono;
    mono.blackAndWhite = {true, 30.0F, 0.0F, -20.0F, 0.0F, 10.0F, -40.0F, 0.0F, 0.0F};
    DevelopSettings toned = mono;
    // Not opposite hues, which would cancel in the midtones.
    toned.colorGrading = {.shadows = {.hue = 250.0F, .saturation = 70.0F},
                          .midtones = {.hue = 80.0F, .saturation = 30.0F},
                          .highlights = {.hue = 30.0F, .saturation = 70.0F}};

    const ProcessingPlan plan = planOf(toned);
    for (const Colour colour : {Colour{0.02F, 0.015F, 0.03F}, Colour{0.7F, 0.2F, 0.1F},
                                Colour{0.6F, 0.8F, 0.9F}, Colour{0.18F, 0.18F, 0.18F}}) {
        CAPTURE(colour[0], colour[1], colour[2]);
        const Colour neutral = developed(mono, colour);
        REQUIRE(chromaOf(neutral) == Approx(0.0F).margin(1e-4F));
        const Colour tinted = developed(toned, colour);
        REQUIRE(chromaOf(tinted) > 0.01F);
        // The grade acts on the grey the mixer made, after it.
        REQUIRE(sameBits(tinted, applyColorGrading(plan.colorAdjustments.grading, neutral)));
        REQUIRE(toOklab(tinted).lightness == Approx(toOklab(neutral).lightness).margin(2e-5F));
    }
    // Dark greys toward the shadows' blue, light ones toward the highlights' red.
    REQUIRE(hueDistance(hueOf(developed(toned, grey(0.005F))), 250.0F) < 5.0F);
    REQUIRE(hueDistance(hueOf(developed(toned, grey(0.9F))), 30.0F) < 5.0F);
}

TEST_CASE("Grading follows the colour controls", "[grading]") {
    DevelopSettings settings = graded({.midtones = {.hue = 30.0F, .saturation = 50.0F}});
    settings.color.saturation = weakestSaturation;
    // Saturation removes the colour, then the grade adds its own: the result
    // has the grade's hue, not grey.
    const Colour out = developed(settings, Colour{0.1F, 0.4F, 0.15F});
    REQUIRE(chromaOf(out) > 0.02F);
    REQUIRE(hueDistance(hueOf(out), 30.0F) < 1.0F);
}

TEST_CASE("The grading rows are pointwise, always apply and share their group", "[grading]") {
    int rows = 0;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (descriptor.group != SettingGroup::ColorGrading) {
            continue;
        }
        INFO(descriptor.key);
        ++rows;
        REQUIRE(descriptor.affects == Stage::Pointwise);
        REQUIRE(descriptor.applies == Applicability::Always);
        REQUIRE(descriptor.range.has_value());
    }
    REQUIRE(rows == 8);
    for (const char* key : {"gradeShadowHue", "gradeMidtoneHue", "gradeHighlightHue"}) {
        REQUIRE(findDescriptor(key)->range->minimum == 0.0);
        REQUIRE(findDescriptor(key)->range->maximum == 360.0);
    }
    for (const char* key :
         {"gradeShadowSaturation", "gradeMidtoneSaturation", "gradeHighlightSaturation"}) {
        REQUIRE(findDescriptor(key)->range->minimum == 0.0);
        REQUIRE(findDescriptor(key)->range->maximum == 100.0);
    }
    REQUIRE(findDescriptor("gradeBalance")->range->minimum == -100.0);
    REQUIRE(findDescriptor("gradeBalance")->range->maximum == 100.0);
    REQUIRE(findDescriptor("gradeBlending")->range->minimum == 0.0);
    REQUIRE(findDescriptor("gradeBlending")->range->maximum == 100.0);
    REQUIRE(ColorGradingSettings{}.blending == 50.0F);
}

TEST_CASE("Grading settings survive a JSON round trip", "[grading][json]") {
    const DevelopSettings settings = graded({.shadows = {.hue = 215.5F, .saturation = 33.0F},
                                             .midtones = {.hue = 12.0F, .saturation = 4.25F},
                                             .highlights = {.hue = 359.0F, .saturation = 100.0F},
                                             .balance = -42.0F,
                                             .blending = 81.5F});
    const std::string text = settingsToJson(settings);
    REQUIRE(text.find("\"gradeShadowHue\": 215.5") != std::string::npos);
    REQUIRE(text.find("\"gradeBalance\": -42") != std::string::npos);
    REQUIRE(text.find("\"gradeBlending\": 81.5") != std::string::npos);
    REQUIRE(applySettingsJson(text, DevelopSettings{}) == settings);
}

namespace {

/// @brief A neutral grey of an Oklab lightness.
Colour greyOfLightness(float lightness) {
    return fromOklab({lightness, 0.0F, 0.0F});
}

/// @brief Grades one colour as the code did before the fade toward white: the whole tint.
Colour unfadedGrade(const ColorGradingPlan& plan, Colour colour) {
    const float luminance = colorspaces::workingLuminance[0] * colour[0] +
                            colorspaces::workingLuminance[1] * colour[1] +
                            colorspaces::workingLuminance[2] * colour[2];
    const ZoneWeights weights = gradeZoneWeights(plan, luminance);
    Oklab lab = toOklab(colour);
    lab.a += weights.shadows * plan.shadowTint.a + weights.midtones * plan.midtoneTint.a +
             weights.highlights * plan.highlightTint.a;
    lab.b += weights.shadows * plan.shadowTint.b + weights.midtones * plan.midtoneTint.b +
             weights.highlights * plan.highlightTint.b;
    return fromOklab(lab);
}

} // namespace

TEST_CASE("The tint fade runs smoothly from whole at L 0.85 to none at white", "[grading][fade]") {
    for (const float lightness : {-1.0F, 0.0F, 0.18F, 0.5F, 0.8F, 0.85F}) {
        CAPTURE(lightness);
        REQUIRE(gradeTintFade(lightness) == 1.0F);
    }
    for (const float lightness : {1.0F, 1.0001F, 1.5F, 40.0F}) {
        CAPTURE(lightness);
        REQUIRE(gradeTintFade(lightness) == 0.0F);
    }
    REQUIRE(gradeTintFade(0.925F) == Approx(0.5F).margin(1e-6F));

    // Continuous and falling: no step anywhere across the fade or at its ends.
    constexpr int steps = 3000;
    float previous = gradeTintFade(0.8F);
    for (int step = 1; step <= steps; ++step) {
        const float lightness = 0.8F + 0.25F * static_cast<float>(step) / steps;
        CAPTURE(lightness);
        const float fade = gradeTintFade(lightness);
        REQUIRE(fade <= previous);
        // Steepest slope is 1.5 / 0.15 = 10 per unit of L.
        REQUIRE(previous - fade <= 10.0F * 0.25F / steps + 1e-6F);
        previous = fade;
    }
}

TEST_CASE("A highlight tint near white keeps every channel in range", "[grading][fade]") {
    // The review's case: a full highlight tint on greys at and near white,
    // which without the fade reached channels of 1.2 to 1.7.
    struct Case {
        float hue;
        Colour colour;
    };
    const std::array<Case, 6> cases{{
        {30.0F, greyOfLightness(1.0F)},
        {90.0F, grey(0.9F)},
        {260.0F, greyOfLightness(0.95F)},
        {30.0F, greyOfLightness(0.95F)},
        {90.0F, greyOfLightness(0.97F)},
        {260.0F, grey(0.9F)},
    }};
    for (const Case& c : cases) {
        const DevelopSettings settings =
            graded({.highlights = {.hue = c.hue, .saturation = strongestGrade}});
        const ColorGradingPlan plan = planOf(settings).colorAdjustments.grading;
        CAPTURE(c.hue, c.colour[0]);
        const float ceiling = std::max({1.0F, c.colour[0], c.colour[1], c.colour[2]});
        const Colour out = developed(settings, c.colour);
        for (const float channel : out) {
            CAPTURE(out[0], out[1], out[2]);
            REQUIRE(channel >= 0.0F);
            REQUIRE(channel <= ceiling);
        }
        // Unfaded, the same grade overshoots: the fade is what holds it.
        const Colour unfaded = unfadedGrade(plan, c.colour);
        if (toOklab(c.colour).lightness < 0.999F) {
            REQUIRE(std::max({unfaded[0], unfaded[1], unfaded[2]}) > ceiling);
        }
    }
}

TEST_CASE("At half saturation every grey stays in range whatever the hue", "[grading][fade]") {
    // Bounds what the fade holds: at full saturation a blue tint can still
    // overshoot a little below white (ADR 034); at half, nothing does.
    for (int hue = 0; hue < 360; hue += 10) {
        const DevelopSettings settings =
            graded({.midtones = {.hue = static_cast<float>(hue), .saturation = 50.0F},
                    .highlights = {.hue = static_cast<float>(hue), .saturation = 50.0F}});
        const ColorGradingPlan plan = planOf(settings).colorAdjustments.grading;
        for (int step = 0; step <= 100; ++step) {
            const Colour colour = greyOfLightness(0.5F + 0.5F * static_cast<float>(step) / 100);
            const Colour out = applyColorGrading(plan, colour);
            CAPTURE(hue, colour[0], out[0], out[1], out[2]);
            REQUIRE(std::min({out[0], out[1], out[2]}) >= 0.0F);
            REQUIRE(std::max({out[0], out[1], out[2]}) <=
                    std::max({1.0F, colour[0], colour[1], colour[2]}));
        }
    }
}

TEST_CASE("A grey at white or above it is left as it is", "[grading][fade]") {
    const DevelopSettings settings =
        graded({.midtones = {.hue = 140.0F, .saturation = 80.0F},
                .highlights = {.hue = 30.0F, .saturation = strongestGrade}});
    const ColorGradingPlan plan = planOf(settings).colorAdjustments.grading;
    for (const Colour colour : {greyOfLightness(1.0F), grey(1.0F), grey(1.5F), grey(16.0F)}) {
        CAPTURE(colour[0]);
        // Not even the Oklab round trip's rounding.
        REQUIRE(sameBits(applyColorGrading(plan, colour), colour));
        REQUIRE(sameBits(developed(settings, colour), colour));
    }
    // A colour, not only a grey, at or above white is left alone too.
    const Colour bright{2.0F, 1.5F, 1.0F};
    REQUIRE(toOklab(bright).lightness >= 1.0F);
    REQUIRE(sameBits(applyColorGrading(plan, bright), bright));
}

TEST_CASE("Below L 0.85 the grade is bit for bit what it was before the fade", "[grading][fade]") {
    const DevelopSettings settings = graded({.shadows = {.hue = 230.0F, .saturation = 100.0F},
                                             .midtones = {.hue = 20.0F, .saturation = 60.0F},
                                             .highlights = {.hue = 90.0F, .saturation = 100.0F},
                                             .balance = 30.0F,
                                             .blending = 70.0F});
    const ColorGradingPlan plan = planOf(settings).colorAdjustments.grading;
    std::vector<Colour> colours{{0.8F, 0.1F, 0.05F},
                                {0.1F, 0.5F, 0.2F},
                                {0.3F, 0.3F, 0.9F},
                                {0.45F, 0.4F, 0.38F},
                                greyOfLightness(0.849F)};
    for (const float luminance : {0.002F, 0.02F, 0.08F, 0.18F, 0.4F, 0.6F}) {
        colours.push_back(grey(luminance));
    }
    for (const Colour colour : colours) {
        CAPTURE(colour[0], colour[1], colour[2]);
        REQUIRE(toOklab(colour).lightness < 0.85F);
        REQUIRE(sameBits(applyColorGrading(plan, colour), unfadedGrade(plan, colour)));
    }
}
