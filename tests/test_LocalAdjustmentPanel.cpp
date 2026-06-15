#include "LocalAdjustmentPanel.h"
#include "TestApp.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <variant>
#include <QDoubleSpinBox>

using Catch::Matchers::WithinAbs;

TEST_CASE("panel adds a default Linear mask and selects it", "[localadj][panel]") {
    testApp();
    LocalAdjustmentPanel panel;
    REQUIRE(panel.localAdjustments().empty());

    panel.addLinearMask();
    const auto list = panel.localAdjustments();
    REQUIRE(list.size() == 1);
    REQUIRE(std::holds_alternative<LinearMask>(list[0].mask));
    REQUIRE(panel.activeIndex() == 0);
}

TEST_CASE("panel geometry fields edit the active mask", "[localadj][panel]") {
    testApp();
    LocalAdjustmentPanel panel;
    panel.addLinearMask();

    // The four geometry spinboxes are named p0x / p0y / p1x / p1y.
    panel.findChild<QDoubleSpinBox*>("p0x")->setValue(0.1);
    panel.findChild<QDoubleSpinBox*>("p0y")->setValue(0.2);
    panel.findChild<QDoubleSpinBox*>("p1x")->setValue(0.6);
    panel.findChild<QDoubleSpinBox*>("p1y")->setValue(0.8);

    const auto& m = std::get<LinearMask>(panel.localAdjustments()[0].mask);
    REQUIRE_THAT(m.p0.x(), WithinAbs(0.1, 1e-6));
    REQUIRE_THAT(m.p0.y(), WithinAbs(0.2, 1e-6));
    REQUIRE_THAT(m.p1.x(), WithinAbs(0.6, 1e-6));
    REQUIRE_THAT(m.p1.y(), WithinAbs(0.8, 1e-6));
}

TEST_CASE("panel round-trips a set list, selects first, and deletes",
          "[localadj][panel]") {
    testApp();
    LocalAdjustmentPanel panel;

    std::vector<LocalAdjustment> in(2);
    in[0].mask     = LinearMask{{0.0, 0.0}, {1.0, 1.0}};
    in[0].exposure = 0.5f;
    in[1].mask     = LinearMask{{0.2, 0.3}, {0.6, 0.7}};
    panel.setLocalAdjustments(in);

    REQUIRE(panel.localAdjustments().size() == 2);
    REQUIRE(panel.activeIndex() == 0);
    REQUIRE(panel.localAdjustments() == in);

    panel.deleteActive();
    REQUIRE(panel.localAdjustments().size() == 1);
}
