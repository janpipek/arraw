#include "develop/GlobalAdjustment.h"
#include "ui/AdjustmentPanel.h"
#include "develop/DemosaicAlgorithm.h"
#include "TestApp.h"
#include <catch2/catch_test_macros.hpp>

#include <QCheckBox>
#include <QComboBox>
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

TEST_CASE("Lens correction toggles are gated by a profile and drive params", "[adjustpanel][lens]") {
    testApp();
    AdjustmentPanel panel;
    auto* dist = panel.findChild<QCheckBox*>("lensCorrectDistortionBox");
    auto* ca = panel.findChild<QCheckBox*>("lensCorrectCABox");
    REQUIRE(dist != nullptr);
    REQUIRE(ca != nullptr);

    // No profile yet → toggles disabled.
    CHECK_FALSE(dist->isEnabled());

    panel.setLensProfileName("Sigma 56mm F1.4 DC DN | Contemporary 018");
    CHECK(dist->isEnabled());
    CHECK(ca->isEnabled());

    int emitted = 0;
    QObject::connect(&panel, &AdjustmentPanel::paramsChanged, [&] { ++emitted; });
    dist->setChecked(true);
    CHECK(panel.params().lensCorrectDistortion);
    CHECK_FALSE(panel.params().lensCorrectVignetting);
    CHECK(emitted >= 1);

    // Clearing the profile disables the toggles again.
    panel.setLensProfileName(QString());
    CHECK_FALSE(dist->isEnabled());
}

// ── Demosaic algorithm combo (issue #22, ADR 0036) ──────────────────────────

TEST_CASE("the demosaic combo round-trips the algorithm through params", "[adjustpanel][demosaic]") {
    testApp();
    AdjustmentPanel panel;
    auto* combo = panel.findChild<QComboBox*>("demosaicCombo");
    REQUIRE(combo != nullptr);

    // Every Bayer algorithm is offered exactly once.
    CHECK(combo->count() == 7);

    // setParams pushes the stored algorithm into the combo selection...
    GlobalAdjustment p;
    p.demosaicAlgorithm = DemosaicAlgorithm::DCB;
    panel.setParams(p);
    CHECK(panel.params().demosaicAlgorithm == DemosaicAlgorithm::DCB);

    // ...and choosing a different entry drives params + emits a commit (one undo
    // entry) carrying the new algorithm, like the lens toggles.
    int committed = 0;
    GlobalAdjustment after;
    QObject::connect(
        &panel,
        &AdjustmentPanel::adjustmentCommitted,
        [&](const GlobalAdjustment&, const GlobalAdjustment& a) {
            ++committed;
            after = a;
        });

    const int vngIndex = combo->findData(static_cast<int>(DemosaicAlgorithm::VNG));
    REQUIRE(vngIndex >= 0);
    combo->setCurrentIndex(vngIndex);

    CHECK(panel.params().demosaicAlgorithm == DemosaicAlgorithm::VNG);
    CHECK(committed >= 1);
    CHECK(after.demosaicAlgorithm == DemosaicAlgorithm::VNG);
}

TEST_CASE("the demosaic combo is disabled for non-Bayer sensors", "[adjustpanel][demosaic]") {
    testApp();
    AdjustmentPanel panel;
    auto* combo = panel.findChild<QComboBox*>("demosaicCombo");
    REQUIRE(combo != nullptr);

    panel.setDemosaicAvailable(true);
    CHECK(combo->isEnabled());

    // X-Trans / Foveon / standard image: the control is shown but disabled, with
    // an explanation rather than offering Bayer labels that would not run.
    panel.setDemosaicAvailable(false);
    CHECK_FALSE(combo->isEnabled());
    CHECK_FALSE(combo->toolTip().isEmpty());
}

TEST_CASE("setParams reflects lens toggle state into the checkboxes", "[adjustpanel][lens]") {
    testApp();
    AdjustmentPanel panel;
    panel.setLensProfileName("Some Lens");

    GlobalAdjustment p;
    p.lensCorrectVignetting = true;
    panel.setParams(p);

    auto* vig = panel.findChild<QCheckBox*>("lensCorrectVignettingBox");
    REQUIRE(vig != nullptr);
    CHECK(vig->isChecked());
    CHECK(panel.params().lensCorrectVignetting);
}
