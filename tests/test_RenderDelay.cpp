#include "RenderDelay.h"

#include <DevelopState.h>

#include <catch2/catch_test_macros.hpp>

using namespace arraw;
using namespace arraw::app;

/// How long the preview holds a render back after an edit (ADR 039): only a
/// noise reduction edit waits, and only on the CPU.

TEST_CASE("A noise reduction edit holds the render back", "[app][preview]") {
    const DevelopState before;
    DevelopState after = before;
    after.settings.noiseReduction.luminance = 40.0F;
    CHECK(renderDelayFor(before, after, false) == noiseReductionRenderDelay);

    after = before;
    after.settings.noiseReduction.colorSmoothness = 80.0F;
    CHECK(renderDelayFor(before, after, false) == noiseReductionRenderDelay);
}

TEST_CASE("Other edits render at once", "[app][preview]") {
    const DevelopState before;
    DevelopState after = before;
    after.settings.tone.exposure = 0.5F;
    CHECK(renderDelayFor(before, after, false).count() == 0);

    // A change that carries a noise reduction edit along with another one is not held.
    after.settings.noiseReduction.color = 10.0F;
    CHECK(renderDelayFor(before, after, false).count() == 0);
}

TEST_CASE("An edit that changes nothing renders at once", "[app][preview]") {
    const DevelopState state;
    CHECK(renderDelayFor(state, state, false).count() == 0);
}

TEST_CASE("A noise reduction edit renders at once on the GPU", "[app][preview]") {
    const DevelopState before;
    DevelopState after = before;
    after.settings.noiseReduction.luminance = 40.0F;
    CHECK(renderDelayFor(before, after, true).count() == 0);
}
