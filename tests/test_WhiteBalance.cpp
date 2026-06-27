// White balance is a per-channel multiplicative gain (docs/adr/0025): black
// stays black by construction, and the Kelvin numbers are blackbody-derived.
// These exercise the CPU gain function directly — no GPU context required.
#include "pipeline/ImagePipeline.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;

TEST_CASE("neutral white balance is identity", "[whitebalance]") {
    const auto g = whiteBalanceGain(5500.0f, 0.0f);
    CHECK_THAT(g[0], WithinAbs(1.0, 1e-3));
    CHECK_THAT(g[1], WithinAbs(1.0, 1e-3));
    CHECK_THAT(g[2], WithinAbs(1.0, 1e-3));
}

TEST_CASE("higher Kelvin warms (boosts red, cuts blue)", "[whitebalance]") {
    const auto warm = whiteBalanceGain(9000.0f, 0.0f);
    CHECK(warm[0] > 1.0f); // red boosted
    CHECK(warm[2] < 1.0f); // blue cut
}

TEST_CASE("lower Kelvin cools (cuts red, boosts blue)", "[whitebalance]") {
    const auto cool = whiteBalanceGain(3200.0f, 0.0f);
    CHECK(cool[0] < 1.0f); // red cut
    CHECK(cool[2] > 1.0f); // blue boosted
}

TEST_CASE("red gain is monotonic in Kelvin", "[whitebalance]") {
    CHECK(whiteBalanceGain(3200.0f, 0.0f)[0] < whiteBalanceGain(5500.0f, 0.0f)[0]);
    CHECK(whiteBalanceGain(5500.0f, 0.0f)[0] < whiteBalanceGain(9000.0f, 0.0f)[0]);
}

// The whole point (docs/adr/0025): the gain is multiplicative, so a black pixel
// can never acquire colour, at any temperature or tint.
TEST_CASE("black stays black under any white balance", "[whitebalance]") {
    const float extremes[][2] = {{2000, -100}, {2000, 100}, {12000, -100}, {12000, 100}};
    for (const auto& e : extremes) {
        const auto g = whiteBalanceGain(e[0], e[1]);
        CHECK(0.0f * g[0] == 0.0f);
        CHECK(0.0f * g[1] == 0.0f);
        CHECK(0.0f * g[2] == 0.0f);
    }
}

TEST_CASE("positive tint adds green, negative removes it", "[whitebalance]") {
    CHECK(whiteBalanceGain(5500.0f, 50.0f)[1] > 1.0f);
    CHECK(whiteBalanceGain(5500.0f, -50.0f)[1] < 1.0f);
}

// The picker inverts the same gain function: a pixel that gain(K,tint)
// neutralises must report back that K and tint (docs/adr/0025).
TEST_CASE("picker round-trips Kelvin and tint", "[whitebalance]") {
    const float cases[][2] = {{3200, 0}, {7500, 0}, {5500, 40}, {4000, -30}, {9000, 25}};
    for (const auto& c : cases) {
        const auto g = whiteBalanceGain(c[0], c[1]);
        // Pre-WB pixel that this gain neutralises to grey.
        const float r = 1.0f / g[0], gr = 1.0f / g[1], b = 1.0f / g[2];
        float k, tint;
        whiteBalanceFromNeutral(r, gr, b, k, tint);
        INFO("K=" << c[0] << " tint=" << c[1] << " -> k=" << k << " tint=" << tint);
        CHECK_THAT(k, WithinAbs(c[0], 50.0));   // within 50 K
        CHECK_THAT(tint, WithinAbs(c[1], 1.0)); // within 1 tint unit
    }
}
