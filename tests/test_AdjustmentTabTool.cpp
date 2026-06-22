#include "AdjustmentTabTool.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("selecting the Masks tab activates the local Mask tool", "[adjustment-tabs][tools]") {
    using Tool = ImageViewport::ActiveTool;

    CHECK(toolForAdjustmentTab(1, 1, 2, Tool::None, true) == Tool::LocalMask);
}

TEST_CASE("selecting the Spots tab activates the Spot tool", "[adjustment-tabs][tools]") {
    using Tool = ImageViewport::ActiveTool;

    CHECK(toolForAdjustmentTab(2, 1, 2, Tool::None, true) == Tool::SpotTool);
}

TEST_CASE("leaving a Mask or Spot tab deactivates its on-image tool", "[adjustment-tabs][tools]") {
    using Tool = ImageViewport::ActiveTool;

    CHECK(toolForAdjustmentTab(0, 1, 2, Tool::LocalMask, true) == Tool::None);
    CHECK(toolForAdjustmentTab(3, 1, 2, Tool::SpotTool, true) == Tool::None);
}

TEST_CASE("non-tool tabs preserve unrelated modal tools", "[adjustment-tabs][tools]") {
    using Tool = ImageViewport::ActiveTool;

    CHECK(toolForAdjustmentTab(0, 1, 2, Tool::Crop, true) == Tool::Crop);
    CHECK(toolForAdjustmentTab(3, 1, 2, Tool::WhiteBalance, true) == Tool::WhiteBalance);
}

TEST_CASE("Mask and Spot tabs stay inactive while tools are disabled", "[adjustment-tabs][tools]") {
    using Tool = ImageViewport::ActiveTool;

    CHECK(toolForAdjustmentTab(1, 1, 2, Tool::None, false) == Tool::None);
    CHECK(toolForAdjustmentTab(2, 1, 2, Tool::LocalMask, false) == Tool::None);
}
