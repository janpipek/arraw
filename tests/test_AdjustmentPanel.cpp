#include "AdjustmentPanel.h"
#include "TestApp.h"
#include <catch2/catch_test_macros.hpp>

#include <QSlider>

// The Grain seed is a hidden per-image identity (docs/adr/0026). It is minted
// the first time Grain is enabled, must survive Amount returning to zero, and is
// cleared only by Reset All. These guarantees live in AdjustmentPanel, so they
// are exercised through the real widget and its grain-amount slider.

TEST_CASE("Grain seed is minted when Amount is first raised", "[adjustpanel][grain]") {
    testApp();
    AdjustmentPanel panel;
    REQUIRE(panel.params().grainSeed == 0);

    auto* grain = panel.findChild<QSlider*>("grainAmountSlider");
    REQUIRE(grain != nullptr);

    grain->setValue(40);
    CHECK(panel.params().grainAmount == 40.0f);
    CHECK(panel.params().grainSeed != 0);
}

TEST_CASE("Grain seed survives Amount returning to zero", "[adjustpanel][grain]") {
    testApp();
    AdjustmentPanel panel;
    auto* grain = panel.findChild<QSlider*>("grainAmountSlider");
    REQUIRE(grain != nullptr);

    grain->setValue(40);
    const auto seed = panel.params().grainSeed;
    REQUIRE(seed != 0);

    grain->setValue(0);
    CHECK(panel.params().grainAmount == 0.0f);
    CHECK(panel.params().grainSeed == seed); // identity persists for the next enable

    grain->setValue(25);
    CHECK(panel.params().grainSeed == seed); // re-enabling does not re-roll it
}

TEST_CASE("Reset All clears the Grain seed", "[adjustpanel][grain]") {
    testApp();
    AdjustmentPanel panel;
    auto* grain = panel.findChild<QSlider*>("grainAmountSlider");
    REQUIRE(grain != nullptr);

    grain->setValue(40);
    REQUIRE(panel.params().grainSeed != 0);

    panel.resetAll();
    CHECK(panel.params().grainAmount == 0.0f);
    CHECK(panel.params().grainSeed == 0);
}

TEST_CASE("setParams mints a seed for a foreign Grain import", "[adjustpanel][grain]") {
    testApp();
    AdjustmentPanel panel;

    GlobalAdjustment foreign; // e.g. a Lightroom sidecar: Grain on, no arraw:GrainSeed
    foreign.grainAmount = 30.0f;
    foreign.grainSeed = 0;
    panel.setParams(foreign);
    CHECK(panel.params().grainSeed != 0);

    GlobalAdjustment seeded;
    seeded.grainAmount = 30.0f;
    seeded.grainSeed = 0xABCDEF01U;
    panel.setParams(seeded);
    CHECK(panel.params().grainSeed == 0xABCDEF01U); // an existing seed is preserved
}
