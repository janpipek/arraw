#include "develop/CurveLut.h"
#include "develop/GlobalAdjustment.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

TEST_CASE("curveLutRgba packs luma/R/G/B LUTs interleaved") {
    GlobalAdjustment p; // identity curves
    const auto rgba = curveLutRgba(p);
    // Identity: entry i maps to i/255 in every channel.
    REQUIRE(rgba[0] == 0.0f);
    for (int c = 0; c < 4; ++c)
        REQUIRE(rgba[255 * 4 + c] == 1.0f);
    REQUIRE(rgba[128 * 4 + 0] == Catch::Approx(128.0f / 255.0f).margin(0.01));
}
