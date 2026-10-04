#include "CurveHistogramRefresh.h"
#include "ProcessingPlan.h"
#include "support/TestImages.h"

#include <CurveHistogram.h>
#include <DevelopState.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace arraw;
using namespace arraw::app;

/// When the preview counts the curve histogram again (ADR 036): a decision taken
/// from the plan through sameAtTap, never from a list of settings of its own.

namespace {

std::shared_ptr<const ImageBuffer> makeLevel() {
    return std::make_shared<const ImageBuffer>(
        test::rainbow({96, 64}, PixelFormat::RgbaF32, workingEncoding));
}

ProcessingPlan planOf(const ImageBuffer& level, const DevelopState& state) {
    return planFor(level, state, curveHistogramRequest);
}

} // namespace

TEST_CASE("Nothing counted yet is never current", "[app][curve][histogram]") {
    const auto level = makeLevel();
    CurveHistogramRefresh refresh;
    CHECK_FALSE(refresh.isCurrent(level, planOf(*level, {})));
}

TEST_CASE("Edits after the curve input keep the histogram current", "[app][curve][histogram]") {
    const auto level = makeLevel();
    CurveHistogramRefresh refresh;
    DevelopState state;
    state.settings.tone.exposure = 0.3F;
    refresh.record(level, planOf(*level, state));

    SECTION("a curve") {
        state.settings.toneCurve.luma.points = {{0.0F, 0.0F}, {0.4F, 0.6F}, {1.0F, 1.0F}};
        state.settings.toneCurve.blue.points = {{0.0F, 0.1F}, {1.0F, 0.9F}};
    }
    SECTION("the shoulder, colour and grading") {
        state.settings.tone.filmicHighlights = 80.0F;
        state.settings.color.saturation = 40.0F;
        state.settings.colorGrading.shadows.saturation = 50.0F;
    }
    CHECK(refresh.isCurrent(level, planOf(*level, state)));
}

TEST_CASE("Edits before the curve input, or of the frame, call for a new count",
          "[app][curve][histogram]") {
    const auto level = makeLevel();
    CurveHistogramRefresh refresh;
    DevelopState state;
    refresh.record(level, planOf(*level, state));

    SECTION("exposure") {
        state.settings.tone.exposure = 0.5F;
    }
    SECTION("Basic Tone") {
        state.settings.tone.shadows = 20.0F;
    }
    SECTION("geometry") {
        state.settings.geometry.straighten = 3.0;
    }
    CHECK_FALSE(refresh.isCurrent(level, planOf(*level, state)));
}

TEST_CASE("Another level, or a cleared record, calls for a new count", "[app][curve][histogram]") {
    const auto level = makeLevel();
    CurveHistogramRefresh refresh;
    refresh.record(level, planOf(*level, {}));
    REQUIRE(refresh.isCurrent(level, planOf(*level, {})));

    // Same size and settings, other pixels: the plan cannot tell, the level can.
    const auto other = makeLevel();
    CHECK_FALSE(refresh.isCurrent(other, planOf(*other, {})));

    refresh.clear();
    CHECK_FALSE(refresh.isCurrent(level, planOf(*level, {})));
}
