#include "develop/LocalAdjustment.h"
#include "develop/GlobalAdjustment.h"
#include "develop/BasicTone.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

// Samples the LUT atlas the way the shader does: encode to the perceptual
// coordinate, then linearly interpolate between the two nearest columns.
static float sampleToneLut(const tone::LutAtlas& atlas, int row, float luminance) {
    const float encoded = std::pow(std::clamp(luminance, 0.0f, 1.0f), 1.0f / tone::kGamma);
    const float pos = encoded * float(tone::kLutSize - 1);
    const int i0 = std::clamp(int(std::floor(pos)), 0, tone::kLutSize - 1);
    const int i1 = std::min(i0 + 1, tone::kLutSize - 1);
    const float frac = pos - float(i0);
    const float lo = atlas.rgba[size_t(row * tone::kLutSize + i0) * 4];
    const float hi = atlas.rgba[size_t(row * tone::kLutSize + i1) * 4];
    return lo + (hi - lo) * frac;
}

TEST_CASE("positive Shadows stretches near-black detail without lifting black", "[tone]") {
    SharedAdjustment adjustment;
    adjustment.shadows = 100.0f;

    const float black = tone::mapLuminance(0.0f, adjustment);
    const float dark1 = tone::mapLuminance(0.0001f, adjustment);
    const float dark2 = tone::mapLuminance(0.0002f, adjustment);

    CHECK(black == 0.0f);
    CHECK(dark1 > 0.0001f);
    CHECK(dark2 > dark1);
    CHECK(dark1 < 0.002f);
}

TEST_CASE("negative Highlights recovers near-white detail without moving white", "[tone]") {
    SharedAdjustment adjustment;
    adjustment.highlights = -100.0f;

    const float nearWhite = tone::mapLuminance(0.99f, adjustment);
    const float white = tone::mapLuminance(1.0f, adjustment);

    CHECK(nearWhite < 0.99f);
    CHECK(nearWhite > 0.9f);
    CHECK(white == 1.0f);
}

TEST_CASE("Exposure moves broad midtones with protected endpoints", "[tone]") {
    SharedAdjustment brighter;
    brighter.exposure = 1.0f;

    CHECK(tone::mapLuminance(0.0f, brighter) == 0.0f);
    CHECK(tone::mapLuminance(1.0f, brighter) == 1.0f);

    const float middleGrey = tone::mapLuminance(0.18f, brighter);
    CHECK_THAT(middleGrey, WithinAbs(0.36f, 0.01f));

    SharedAdjustment darker;
    darker.exposure = -1.0f;
    CHECK_THAT(tone::mapLuminance(middleGrey, darker), WithinAbs(0.18f, 0.001f));

    const float midtoneMove = tone::mapLuminance(0.5f, brighter) - 0.5f;
    const float shadowMove = tone::mapLuminance(0.01f, brighter) - 0.01f;
    const float highlightMove = tone::mapLuminance(0.99f, brighter) - 0.99f;
    CHECK(midtoneMove > shadowMove);
    CHECK(midtoneMove > highlightMove);
}

TEST_CASE("Contrast expands perceptual midtones without moving endpoints", "[tone]") {
    SharedAdjustment stronger;
    stronger.contrast = 100.0f;

    CHECK(tone::mapLuminance(0.0f, stronger) == 0.0f);
    CHECK(tone::mapLuminance(1.0f, stronger) == 1.0f);

    constexpr float perceptualMiddle = 0.21763764f; // pow(0.5, 2.2)
    CHECK_THAT(tone::mapLuminance(perceptualMiddle, stronger), WithinAbs(perceptualMiddle, 0.0001f));
    CHECK(tone::mapLuminance(0.1f, stronger) < 0.1f);
    CHECK(tone::mapLuminance(0.5f, stronger) > 0.5f);

    SharedAdjustment weaker;
    weaker.contrast = -100.0f;
    const float contrasted = tone::mapLuminance(0.1f, stronger);
    CHECK_THAT(tone::mapLuminance(contrasted, weaker), WithinAbs(0.1f, 0.0001f));
}

TEST_CASE("Blacks deliberately controls the black clipping point", "[tone]") {
    SharedAdjustment lifted;
    lifted.blacks = 100.0f;
    CHECK(tone::mapLuminance(0.0f, lifted) > 0.0f);
    CHECK(tone::mapLuminance(0.0f, lifted) < 0.02f);
    CHECK_THAT(tone::mapLuminance(0.5f, lifted), WithinAbs(0.5f, 0.001f));

    SharedAdjustment clipped;
    clipped.blacks = -100.0f;
    CHECK(tone::mapLuminance(0.001f, clipped) == 0.0f);
}

TEST_CASE("Whites deliberately controls recoverable white headroom", "[tone]") {
    SharedAdjustment clipped;
    clipped.whites = 100.0f;
    CHECK(tone::mapLuminance(0.9f, clipped) > 1.0f);
    CHECK(tone::mapLuminance(1.0f, clipped) > 1.0f);
    CHECK_THAT(tone::mapLuminance(0.1f, clipped), WithinAbs(0.1f, 0.001f));

    SharedAdjustment recovered;
    recovered.whites = -100.0f;
    CHECK(tone::mapLuminance(1.0f, recovered) < 1.0f);
    CHECK(tone::mapLuminance(1.03f, recovered) < 1.0f);
}

TEST_CASE("Basic Tone LUT carries the tested global curve", "[tone]") {
    GlobalAdjustment adjustment;
    adjustment.shadows = 100.0f;
    const tone::LutAtlas atlas = tone::makeLutAtlas(adjustment);

    // Columns are indexed in the perceptual (gamma-encoded) domain.
    constexpr int index = 32;
    const float input = tone::lutIndexToLuminance(index);
    const float stored = atlas.rgba[index * 4];
    CHECK_THAT(stored, WithinAbs(tone::mapLuminance(input, adjustment), 1e-6f));
}

TEST_CASE("Basic Tone LUT gives each Local Adjustment its own curve row", "[tone]") {
    GlobalAdjustment adjustment;
    LocalAdjustment local;
    local.whites = -100.0f;
    adjustment.localAdjustments.push_back(local);
    const tone::LutAtlas atlas = tone::makeLutAtlas(adjustment);

    constexpr int index = tone::kLutSize - 1;
    constexpr int localRow = 1;
    const float stored = atlas.rgba[(localRow * tone::kLutSize + index) * 4];
    CHECK(stored < 1.0f);
    CHECK_THAT(stored, WithinAbs(tone::mapLuminance(1.0f, local), 1e-6f));
}

TEST_CASE("Basic Tone LUT resolves deep-shadow detail the shader samples", "[tone]") {
    GlobalAdjustment adjustment;
    adjustment.shadows = 100.0f; // works hardest in the darkest tones
    const tone::LutAtlas atlas = tone::makeLutAtlas(adjustment);

    // Perceptual indexing spends columns where the controls act, so a GPU-style
    // lookup tracks the exact model even in near-black values that a
    // linear-indexed LUT would have collapsed into its first one or two columns.
    for (const float luminance : {0.0005f, 0.002f, 0.01f}) {
        const float sampled = sampleToneLut(atlas, 0, luminance);
        const float exact = tone::mapLuminance(luminance, adjustment);
        CHECK_THAT(sampled, WithinAbs(exact, 0.002f));
    }

    // Distinct deep-shadow inputs remain distinguishable after the lookup.
    CHECK(sampleToneLut(atlas, 0, 0.002f) > sampleToneLut(atlas, 0, 0.0005f));
}

TEST_CASE("Basic Tone controls remain monotonic at their extremes", "[tone]") {
    std::vector<std::pair<const char*, SharedAdjustment>> scenarios;
    auto addExtremes =
        [&](const char* negativeName, const char* positiveName, float SharedAdjustment::* field) {
            SharedAdjustment negative;
            negative.*field = -100.0f;
            scenarios.emplace_back(negativeName, negative);
            SharedAdjustment positive;
            positive.*field = 100.0f;
            scenarios.emplace_back(positiveName, positive);
        };
    addExtremes("Contrast -100", "Contrast +100", &SharedAdjustment::contrast);
    addExtremes("Highlights -100", "Highlights +100", &SharedAdjustment::highlights);
    addExtremes("Shadows -100", "Shadows +100", &SharedAdjustment::shadows);
    addExtremes("Whites -100", "Whites +100", &SharedAdjustment::whites);
    addExtremes("Blacks -100", "Blacks +100", &SharedAdjustment::blacks);
    SharedAdjustment exposureDown;
    exposureDown.exposure = -5.0f;
    scenarios.emplace_back("Exposure -5 EV", exposureDown);
    SharedAdjustment exposureUp;
    exposureUp.exposure = 5.0f;
    scenarios.emplace_back("Exposure +5 EV", exposureUp);

    for (const auto& [name, adjustment] : scenarios) {
        DYNAMIC_SECTION(name) {
            float previous = tone::mapLuminance(0.0f, adjustment);
            for (int i = 1; i <= 4096; ++i) {
                const float current = tone::mapLuminance(i / 4096.0f, adjustment);
                REQUIRE(current >= previous - 1e-6f);
                previous = current;
            }
        }
    }
}
