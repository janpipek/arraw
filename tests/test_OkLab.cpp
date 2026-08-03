#include "pipeline/OkLab.h"

#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using colour::Rgb;

namespace {
float chroma(const Rgb& rgb) {
    const colour::Lab lab = colour::toOklab(rgb);
    return std::sqrt(lab.a * lab.a + lab.b * lab.b);
}

float lightness(const Rgb& rgb) {
    return colour::toOklab(rgb).L;
}

using Mix = std::array<float, 8>;
constexpr Mix kFlatMix = {};

enum Band { Red, Orange, Yellow, Green, Aqua, Blue, Purple, Magenta };

// Assert the conversion is achromatic, then return the single grey value.
float grey(const Rgb& c) {
    REQUIRE_THAT(c[1], WithinAbs(c[0], 1e-6f));
    REQUIRE_THAT(c[2], WithinAbs(c[0], 1e-6f));
    return c[0];
}
} // namespace

// ── Oklab round trip (the SPOT contract: one tested transform feeds the GPU) ──

TEST_CASE("Oklab round-trips linear Rec.2020 colours", "[oklab]") {
    const Rgb samples[] = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 1.0f},
        {0.18f, 0.18f, 0.18f},
        {0.8f, 0.1f, 0.05f},
        {0.05f, 0.4f, 0.7f},
        {0.9f, 0.85f, 0.1f},
    };
    for (const Rgb& c : samples) {
        const Rgb back = colour::fromOklab(colour::toOklab(c));
        CHECK_THAT(back[0], WithinAbs(c[0], 1e-4f));
        CHECK_THAT(back[1], WithinAbs(c[1], 1e-4f));
        CHECK_THAT(back[2], WithinAbs(c[2], 1e-4f));
    }
}

TEST_CASE("the neutral grey axis carries no Oklab chroma", "[oklab]") {
    for (float v : {0.05f, 0.18f, 0.5f, 1.0f}) {
        const colour::Lab lab = colour::toOklab({v, v, v});
        CHECK_THAT(lab.a, WithinAbs(0.0f, 1e-4f));
        CHECK_THAT(lab.b, WithinAbs(0.0f, 1e-4f));
    }
}

// ── Saturation: scales chroma, holds lightness and hue ───────────────────────

TEST_CASE("Saturation at 0 is identity", "[oklab][saturation]") {
    const Rgb c{0.6f, 0.2f, 0.3f};
    const Rgb out = colour::applySaturation(c, 0.0f);
    CHECK_THAT(out[0], WithinAbs(c[0], 1e-5f));
    CHECK_THAT(out[1], WithinAbs(c[1], 1e-5f));
    CHECK_THAT(out[2], WithinAbs(c[2], 1e-5f));
}

TEST_CASE("positive Saturation raises chroma while holding lightness", "[oklab][saturation]") {
    const Rgb c{0.5f, 0.25f, 0.2f};
    const Rgb out = colour::applySaturation(c, 0.5f);
    CHECK(chroma(out) > chroma(c));
    CHECK_THAT(lightness(out), WithinAbs(lightness(c), 1e-4f)); // the whole point
}

TEST_CASE("Saturation -1 fully desaturates to neutral", "[oklab][saturation]") {
    const Rgb out = colour::applySaturation({0.5f, 0.25f, 0.2f}, -1.0f);
    CHECK_THAT(chroma(out), WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("Saturation leaves a neutral pixel neutral", "[oklab][saturation]") {
    const Rgb out = colour::applySaturation({0.4f, 0.4f, 0.4f}, 0.8f);
    CHECK_THAT(out[0], WithinAbs(out[1], 1e-5f));
    CHECK_THAT(out[1], WithinAbs(out[2], 1e-5f));
}

// ── Vibrance: like Saturation but protects already-vivid colours ─────────────

TEST_CASE("Vibrance moves muted colours more than vivid ones", "[oklab][vibrance]") {
    const Rgb muted{0.45f, 0.40f, 0.38f};
    const Rgb vivid{0.85f, 0.10f, 0.05f};

    const float mutedGain = chroma(colour::applyVibrance(muted, 0.5f)) / chroma(muted);
    const float vividGain = chroma(colour::applyVibrance(vivid, 0.5f)) / chroma(vivid);

    CHECK(mutedGain > vividGain); // vibrance protects the saturated colour
    CHECK(vividGain > 1.0f);      // but still nudges it
}

TEST_CASE("Vibrance at 0 is identity", "[oklab][vibrance]") {
    const Rgb c{0.6f, 0.2f, 0.3f};
    const Rgb out = colour::applyVibrance(c, 0.0f);
    CHECK_THAT(out[0], WithinAbs(c[0], 1e-5f));
}

// ── Highlight roll-off shoulder (scalar luminance map) ───────────────────────

TEST_CASE("shoulder at amount 0 is the identity, even above white", "[filmic]") {
    for (float y : {0.0f, 0.18f, 0.5f, 1.0f, 2.5f, 8.0f})
        CHECK_THAT(colour::shoulderMap(y, 0.0f), WithinAbs(y, 1e-6f));
}

TEST_CASE("shoulder anchors black, is monotonic, and caps at white", "[filmic]") {
    const float amount = 0.7f;
    CHECK_THAT(colour::shoulderMap(0.0f, amount), WithinAbs(0.0f, 1e-6f));

    float prev = -1.0f;
    for (float y = 0.0f; y <= 16.0f; y += 0.05f) {
        const float mapped = colour::shoulderMap(y, amount);
        CHECK(mapped > prev);    // strictly increasing
        CHECK(mapped < 1.0001f); // never exceeds display white
        prev = mapped;
    }
}

TEST_CASE("shoulder barely touches midtones but compresses headroom", "[filmic]") {
    const float amount = 0.6f;
    CHECK_THAT(colour::shoulderMap(0.18f, amount), WithinAbs(0.18f, 0.02f));
    CHECK(colour::shoulderMap(4.0f, amount) < 1.0f); // headroom rolled into range
    CHECK(colour::shoulderMap(4.0f, amount) > 0.9f); // ...but kept bright
}

// ── Full roll-off: shoulder + path to white ──────────────────────────────────

TEST_CASE("roll-off at amount 0 is identity", "[filmic]") {
    const Rgb c{1.4f, 0.3f, 0.2f};
    const Rgb out = colour::applyFilmicHighlights(c, 0.0f);
    CHECK_THAT(out[0], WithinAbs(c[0], 1e-6f));
    CHECK_THAT(out[1], WithinAbs(c[1], 1e-6f));
    CHECK_THAT(out[2], WithinAbs(c[2], 1e-6f));
}

TEST_CASE("a blown saturated highlight rolls down in luma and fades toward white", "[filmic]") {
    const Rgb hotRed{3.0f, 0.4f, 0.25f}; // well above white, strongly coloured
    const Rgb out = colour::applyFilmicHighlights(hotRed, 0.8f);

    CHECK(lightness(out) < lightness(hotRed)); // shoulder pulled it down
    CHECK(chroma(out) < chroma(hotRed));       // path to white desaturated it
}

TEST_CASE("roll-off leaves a midtone essentially untouched", "[filmic]") {
    const Rgb mid{0.22f, 0.18f, 0.15f};
    const Rgb out = colour::applyFilmicHighlights(mid, 0.8f);
    CHECK_THAT(out[0], WithinAbs(mid[0], 0.02f));
    CHECK_THAT(out[1], WithinAbs(mid[1], 0.02f));
    CHECK_THAT(out[2], WithinAbs(mid[2], 0.02f));
}

// ── Black & White hue mixer (docs/adr/0048) ──────────────────────────────────

TEST_CASE("B&W conversion is achromatic for any input and mix", "[bw]") {
    Mix mix{};
    mix[Blue] = -80.0f;
    mix[Red] = 60.0f;
    for (const Rgb& c : {Rgb{0.6f, 0.06f, 0.05f}, Rgb{0.05f, 0.07f, 0.6f}, Rgb{0.2f, 0.5f, 0.1f}}) {
        const Rgb out = colour::applyBlackAndWhite(c, mix);
        CHECK_THAT(out[1], WithinAbs(out[0], 1e-6f));
        CHECK_THAT(out[2], WithinAbs(out[0], 1e-6f));
    }
}

TEST_CASE("a neutral pixel is unchanged by any B&W mix", "[bw]") {
    Mix loud{};
    loud[Blue] = 100.0f;
    loud[Red] = -100.0f;
    const Rgb out = colour::applyBlackAndWhite({0.4f, 0.4f, 0.4f}, loud);
    CHECK_THAT(grey(out), WithinAbs(0.4f, 1e-6f)); // neutrals never shift
}

TEST_CASE("an all-zero mix yields the Rec.2020 luminance grey", "[bw]") {
    const Rgb c{0.05f, 0.07f, 0.6f};
    const float expected = 0.2627f * c[0] + 0.6780f * c[1] + 0.0593f * c[2];
    CHECK_THAT(grey(colour::applyBlackAndWhite(c, kFlatMix)), WithinAbs(expected, 1e-6f));
}

TEST_CASE("a band darkens its own hue and leaves other hues untouched", "[bw]") {
    Mix blueDown{};
    blueDown[Blue] = -100.0f;
    const Rgb blue{0.05f, 0.07f, 0.6f};
    const Rgb red{0.6f, 0.06f, 0.05f};

    // The red-filter-darkens-the-sky test: pulling Blue down darkens a blue pixel...
    CHECK(
        grey(colour::applyBlackAndWhite(blue, blueDown))
        < grey(colour::applyBlackAndWhite(blue, kFlatMix)));
    // ...while a red pixel, outside the Blue band, is untouched.
    CHECK_THAT(
        grey(colour::applyBlackAndWhite(red, blueDown)),
        WithinAbs(grey(colour::applyBlackAndWhite(red, kFlatMix)), 1e-6f));
}

TEST_CASE("a positive band lightens its hue", "[bw]") {
    Mix blueUp{};
    blueUp[Blue] = 100.0f;
    const Rgb blue{0.05f, 0.07f, 0.6f};
    CHECK(
        grey(colour::applyBlackAndWhite(blue, blueUp))
        > grey(colour::applyBlackAndWhite(blue, kFlatMix)));
}

TEST_CASE("the mixer shift scales with saturation", "[bw]") {
    Mix blueUp{};
    blueUp[Blue] = 100.0f;
    const Rgb fullBlue{0.05f, 0.07f, 0.6f};
    const Rgb halfBlue{0.30f, 0.32f, 0.60f}; // same hue, lower saturation

    const float fullGain = grey(colour::applyBlackAndWhite(fullBlue, blueUp))
                           / grey(colour::applyBlackAndWhite(fullBlue, kFlatMix));
    const float halfGain = grey(colour::applyBlackAndWhite(halfBlue, blueUp))
                           / grey(colour::applyBlackAndWhite(halfBlue, kFlatMix));
    CHECK(fullGain > halfGain); // a saturated blue moves further than a muted one
    CHECK(halfGain > 1.0f);     // ...but the muted one still moves
}

// ── Colour Grading (docs/adr/0052) ───────────────────────────────────────────

namespace {
using Zone3 = std::array<float, 3>; // [Shadows, Midtones, Highlights]
constexpr Zone3 kNoZone = {};
} // namespace

TEST_CASE("Colour Grading with zero saturation is the exact identity", "[grade]") {
    // Hue, Balance and Blending must do nothing without saturation to carry them.
    const Rgb samples[] = {{0.02f, 0.03f, 0.05f}, {0.4f, 0.4f, 0.4f}, {0.8f, 0.1f, 0.05f}};
    const Zone3 hue = {30.0f, 210.0f, 300.0f};
    for (const Rgb& c : samples) {
        const Rgb out = colour::applyColourGrading(c, hue, kNoZone, -40.0f, 20.0f);
        CHECK_THAT(out[0], WithinAbs(c[0], 1e-6f));
        CHECK_THAT(out[1], WithinAbs(c[1], 1e-6f));
        CHECK_THAT(out[2], WithinAbs(c[2], 1e-6f));
    }
}

TEST_CASE("Colour Grading tints a neutral grey while holding lightness", "[grade]") {
    // The feature's reason to exist: sepia/split-toning on a neutral (B&W) signal.
    const Rgb grey{0.3f, 0.3f, 0.3f};
    const Zone3 sat = {60.0f, 60.0f, 60.0f};
    const Rgb out = colour::applyColourGrading(grey, {40.0f, 40.0f, 40.0f}, sat, 0.0f, 50.0f);
    CHECK(chroma(out) > 0.01f);                                    // now carries colour
    CHECK_THAT(lightness(out), WithinAbs(lightness(grey), 1e-4f)); // lightness held
}

TEST_CASE("the Shadows zone tints dark tones more than bright tones", "[grade]") {
    const Zone3 shadowOnly = {80.0f, 0.0f, 0.0f}; // saturation only in Shadows
    const Rgb dark{0.03f, 0.03f, 0.03f};
    const Rgb bright{0.8f, 0.8f, 0.8f};
    const float darkChroma = chroma(
        colour::applyColourGrading(dark, kNoZone, shadowOnly, 0.0f, 50.0f));
    const float brightChroma = chroma(
        colour::applyColourGrading(bright, kNoZone, shadowOnly, 0.0f, 50.0f));
    CHECK(darkChroma > brightChroma);
}

TEST_CASE("the Highlights zone tints bright tones more than dark tones", "[grade]") {
    const Zone3 highlightOnly = {0.0f, 0.0f, 80.0f};
    const Rgb dark{0.03f, 0.03f, 0.03f};
    const Rgb bright{0.8f, 0.8f, 0.8f};
    const float darkChroma = chroma(
        colour::applyColourGrading(dark, kNoZone, highlightOnly, 0.0f, 50.0f));
    const float brightChroma = chroma(
        colour::applyColourGrading(bright, kNoZone, highlightOnly, 0.0f, 50.0f));
    CHECK(brightChroma > darkChroma);
}

TEST_CASE("the grade hue steers the direction of the Oklab tint", "[grade]") {
    const Rgb grey{0.3f, 0.3f, 0.3f};
    const Zone3 sat = {70.0f, 70.0f, 70.0f};
    // Hue 0° pushes along +a (b ~ 0); hue 90° pushes along +b (a ~ 0).
    const colour::Lab alongA = colour::toOklab(
        colour::applyColourGrading(grey, {0.0f, 0.0f, 0.0f}, sat, 0.0f, 50.0f));
    const colour::Lab alongB = colour::toOklab(
        colour::applyColourGrading(grey, {90.0f, 90.0f, 90.0f}, sat, 0.0f, 50.0f));
    CHECK(alongA.a > 0.01f);
    CHECK_THAT(alongA.b, WithinAbs(0.0f, 1e-3f));
    CHECK(alongB.b > 0.01f);
    CHECK_THAT(alongB.a, WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("Balance shifts the shadow/highlight crossover, Lightroom-style", "[grade]") {
    const Zone3 shadowOnly = {80.0f, 0.0f, 0.0f};
    const Rgb mid{0.35f, 0.35f, 0.35f};
    // Sign follows crs:ColorGradeBalance: negative hands more of the tonal range to
    // the Shadows zone (a midtone picks up more of its tint), positive hands it to
    // the Highlights (the midtone picks up less).
    const auto tint = [&](float balance) {
        return chroma(colour::applyColourGrading(mid, kNoZone, shadowOnly, balance, 50.0f));
    };
    CHECK(tint(-100.0f) > tint(0.0f));
    CHECK(tint(100.0f) < tint(0.0f));
}
