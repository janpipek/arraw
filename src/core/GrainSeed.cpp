#include <EffectsSettings.h>

#include <random>

using namespace arraw;

namespace {

/// @brief Tells whether grain is on as the plan sees it: not an amount it would clamp to zero, nor
/// not a number.
bool grainIsOn(const GrainSettings& grain) {
    return grain.amount > 0.0F;
}

} // namespace

std::uint32_t arraw::chooseGrainSeed(const GrainSettings& previous, const GrainSettings& next,
                                     const GrainEntropy& entropy) {
    if (grainIsOn(previous) || !grainIsOn(next) || next.seed != 0) {
        return next.seed;
    }
    std::uint32_t drawn = 0;
    if (entropy) {
        drawn = entropy();
    } else {
        std::random_device device;
        drawn = static_cast<std::uint32_t>(device());
    }
    // Zero means "none chosen", so it is never chosen; one stands in for it.
    return drawn != 0 ? drawn : 1U;
}
