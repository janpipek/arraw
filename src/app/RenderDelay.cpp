#include "RenderDelay.h"

using namespace arraw;
using namespace arraw::app;

std::chrono::milliseconds arraw::app::renderDelayFor(const DevelopState& before,
                                                     const DevelopState& after) {
    if (before.settings.noiseReduction == after.settings.noiseReduction) {
        return std::chrono::milliseconds{0};
    }
    DevelopState sameNoise = after;
    sameNoise.settings.noiseReduction = before.settings.noiseReduction;
    return sameNoise == before ? noiseReductionRenderDelay : std::chrono::milliseconds{0};
}
