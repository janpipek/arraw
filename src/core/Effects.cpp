#include "Effects.h"

#include "ColorAdjustments.h"
#include "ProcessingPlan.h"
#include "ProgressScope.h"
#include "RowBands.h"
#include "TimingTrace.h"

#include <Progress.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

using namespace arraw;

namespace {

/// @brief Resolves the vignette settings; off when the amount is zero.
VignettePlan vignettePlanFor(const VignetteSettings& settings) {
    const float amount =
        clampedSetting(settings.amount, darkestVignette, lightestVignette, "vignette amount");
    const float midpoint = clampedSetting(settings.midpoint, minimumVignetteShape,
                                          maximumVignetteShape, "vignette midpoint");
    const float feather = clampedSetting(settings.feather, minimumVignetteShape,
                                         maximumVignetteShape, "vignette feather");
    if (amount == 0.0F) {
        return {};
    }

    VignettePlan plan;
    plan.active = true;
    plan.lightens = amount > 0.0F;
    plan.stops = std::abs(amount) / lightestVignette * strongestVignetteStops;
    plan.inner = midpoint / maximumVignetteShape * vignetteInnermostReach;
    const float softness = feather / maximumVignetteShape;
    plan.hardEdge = softness == 0.0F;
    if (plan.hardEdge) {
        plan.outer = plan.inner;
    } else {
        plan.outer = std::max(plan.inner + (1.0F - plan.inner) * softness,
                              plan.inner + narrowestVignetteFalloff);
    }
    return plan;
}

} // namespace

EffectsPlan arraw::effectsPlanFor(const EffectsSettings& settings) {
    EffectsPlan plan;
    plan.vignette = vignettePlanFor(settings.vignette);
    plan.grain = grainPlanFor(settings.grain);
    return plan;
}

EffectsPlacement arraw::effectsPlacementOf(const EffectsPlan& plan, const FrameMapping& mapping) {
    EffectsPlacement placement;
    placement.mapping = mapping;
    if (plan.grain.active) {
        placement.grain = grainPlacementOf(plan.grain, mapping);
    }
    return placement;
}

FramePoint arraw::framePointOf(const FrameMapping& mapping, std::uint32_t column,
                               std::uint32_t row) {
    return {static_cast<float>(mapping.origin[0] + (column + 0.5) * mapping.step[0]),
            static_cast<float>(mapping.origin[1] + (row + 0.5) * mapping.step[1])};
}

float arraw::vignetteWeight(const VignettePlan& plan, FramePoint point) {
    const float across = 2.0F * point.x - 1.0F;
    const float down = 2.0F * point.y - 1.0F;
    const float radius = std::sqrt((across * across + down * down) * 0.5F);
    if (plan.hardEdge) {
        return radius >= plan.inner ? 1.0F : 0.0F;
    }
    return smoothstep(plan.inner, plan.outer, radius);
}

Colour arraw::applyVignette(const VignettePlan& plan, Colour colour, float weight) {
    // Exactly the colour where there is no falloff: neither a gain of one nor
    // a round trip through the perceptual coordinate is spent on it.
    if (!(weight > 0.0F)) {
        return colour;
    }
    if (!plan.lightens) {
        const float gain = std::exp2(-plan.stops * weight);
        return {colour[0] * gain, colour[1] * gain, colour[2] * gain};
    }
    const float keep = std::exp2(-plan.stops * weight / 2.2F);
    const auto screen = [keep](float value) {
        return fromPerceptualSigned(1.0F - (1.0F - toPerceptualSigned(value)) * keep);
    };
    return {screen(colour[0]), screen(colour[1]), screen(colour[2])};
}

Colour arraw::applyGrain(Colour colour, float grain) {
    // Exactly the colour where a band-limited grain has nothing left to add.
    if (grain == 0.0F) {
        return colour;
    }
    const auto add = [grain](float value) {
        return fromPerceptualSigned(toPerceptualSigned(value) + grain);
    };
    return {add(colour[0]), add(colour[1]), add(colour[2])};
}

Colour arraw::effectsPixel(const EffectsPlan& plan, const EffectsPlacement& placement,
                           std::uint32_t column, std::uint32_t row, Colour colour) {
    if (plan.vignette.active) {
        const FramePoint point = framePointOf(placement.mapping, column, row);
        colour = applyVignette(plan.vignette, colour, vignetteWeight(plan.vignette, point));
    }
    if (plan.grain.active) {
        colour = applyGrain(colour, grainAt(plan.grain, placement.grain, column, row));
    }
    return colour;
}

ImageBuffer arraw::applyEffects(ImageBuffer pixels, const EffectsPlan& plan,
                                const FrameMapping& mapping) {
    const detail::TimingSpan timing("cpu.effects");
    const ImageSize size = pixels.size();
    const EffectsPlacement placement = effectsPlacementOf(plan, mapping);
    const auto samples = pixels.samples<float>();
    const detail::ProgressSpan progress(ProgressStep::Effects);
    detail::forEachRowInTurn(size.height, size.width, [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t row = first; row < last; ++row) {
            for (std::uint32_t column = 0; column < size.width; ++column) {
                float* pixel = &samples[(static_cast<std::size_t>(row) * size.width + column) * 4];
                const Colour result =
                    effectsPixel(plan, placement, column, row, {pixel[0], pixel[1], pixel[2]});
                pixel[0] = result[0];
                pixel[1] = result[1];
                pixel[2] = result[2];
            }
        }
    });
    return pixels;
}
