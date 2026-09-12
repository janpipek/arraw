#include "render/FocusPeaking.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Focus Peaking thresholds are ordered from least to most sensitive", "[peaking]") {
    // Low is the least sensitive (highest threshold, fewest edges flagged);
    // High is the most sensitive (lowest threshold, most edges flagged).
    CHECK(kFocusPeakingThresholds[int(FocusPeakingSensitivity::Low)]
          > kFocusPeakingThresholds[int(FocusPeakingSensitivity::Mid)]);
    CHECK(kFocusPeakingThresholds[int(FocusPeakingSensitivity::Mid)]
          > kFocusPeakingThresholds[int(FocusPeakingSensitivity::High)]);
}

TEST_CASE("Focus Peaking thresholds stay in a sane normalised range", "[peaking]") {
    for (float t : kFocusPeakingThresholds) {
        CHECK(t > 0.0f);
        CHECK(t < 1.5f); // a 3x3 Sobel on [0,1] luma tops out around sqrt(2)*4/4 ≈ 1.41
    }
}
