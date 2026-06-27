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
