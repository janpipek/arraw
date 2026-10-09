#include "LocalPlan.h"

#include "ColorAdjustments.h"

#include <LocalAdjustments.h>
#include <PresenceSettings.h>
#include <ToneSettings.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <variant>

using namespace arraw;

namespace {

/// @brief Checks that a control's name and its row of the table agree.
constexpr bool rowIs(LocalControl control, std::string_view key) {
    return localAdjustmentDescriptors[indexOf(control)].key == key;
}

static_assert(localControlCount == 13, "the plan's arrays are sized for thirteen controls");
static_assert(rowIs(LocalControl::RelativeTemperature, "relativeTemperature") &&
              rowIs(LocalControl::RelativeTint, "relativeTint") &&
              rowIs(LocalControl::Exposure, "exposure") &&
              rowIs(LocalControl::Contrast, "contrast") &&
              rowIs(LocalControl::Highlights, "highlights") &&
              rowIs(LocalControl::Shadows, "shadows") && rowIs(LocalControl::Whites, "whites") &&
              rowIs(LocalControl::Blacks, "blacks") && rowIs(LocalControl::Texture, "texture") &&
              rowIs(LocalControl::Clarity, "clarity") && rowIs(LocalControl::Dehaze, "dehaze") &&
              rowIs(LocalControl::Saturation, "saturation") &&
              rowIs(LocalControl::Vibrance, "vibrance"));

/// @brief Checks that exactly the controls before Saturation act before the curve-input tap.
constexpr bool preTapRowsFirst() {
    for (std::size_t row = 0; row < localControlCount; ++row) {
        if (localAdjustmentDescriptors[row].beforeCurveTap() != (row < preTapControlCount)) {
            return false;
        }
    }
    return true;
}

static_assert(preTapRowsFirst(), "the controls before the tap must be the table's first rows");

/// @brief Resolves one linear mask into the coefficients of its `t`.
void resolveLinear(const LinearMask& shape, ImageSize source, LocalMaskPlan& plan) {
    const double width = source.width;
    const double height = source.height;
    const double longEdge = std::max(width, height);
    // The ends in long-edge units: the isotropic metric of ADR 044, section 4.
    const double ax = shape.from.u * width / longEdge;
    const double ay = shape.from.v * height / longEdge;
    const double dx = shape.to.u * width / longEdge - ax;
    const double dy = shape.to.v * height / longEdge - ay;
    const double lengthSquared = dx * dx + dy * dy;
    plan.kind = LocalMaskKind::Linear;
    plan.alpha = static_cast<float>(dx / (longEdge * lengthSquared));
    plan.beta = static_cast<float>(dy / (longEdge * lengthSquared));
    plan.gamma = static_cast<float>(-(ax * dx + ay * dy) / lengthSquared);
}

/// @brief Resolves one radial mask into its centre, matrix and inner distance.
void resolveRadial(const RadialMask& shape, ImageSize source, LocalMaskPlan& plan) {
    const double width = source.width;
    const double height = source.height;
    const double longEdge = std::max(width, height);
    const double angle = static_cast<double>(shape.angle) * std::numbers::pi / 180.0;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    plan.kind = LocalMaskKind::Radial;
    plan.centreX = static_cast<float>(shape.centre.u * width);
    plan.centreY = static_cast<float>(shape.centre.v * height);
    // q = R(-theta) (P - C) / radii, with P - C in pixels divided by the long edge.
    plan.matrix = {static_cast<float>(cosine / (shape.radiusX * longEdge)),
                   static_cast<float>(sine / (shape.radiusX * longEdge)),
                   static_cast<float>(-sine / (shape.radiusY * longEdge)),
                   static_cast<float>(cosine / (shape.radiusY * longEdge))};
    // One pixel of the rendered source along the short axis, in the units of the radii.
    const double pixel = 1.0 / longEdge / std::min(shape.radiusX, shape.radiusY);
    plan.inner = static_cast<float>(1.0 - std::max(static_cast<double>(shape.feather), pixel));
}

} // namespace

LocalPlan arraw::localPlanFor(const DevelopState& state, ImageSize source) {
    LocalPlan plan;
    if (state.localAdjustments.empty()) {
        return plan;
    }
    if (source.empty()) {
        throw std::invalid_argument("Cannot resolve local adjustments against an empty photograph");
    }
    for (const LocalAdjustment& original : state.localAdjustments) {
        if (!original.enabled) {
            continue;
        }
        // Clamped and checked as an edit would, whatever built the state; the name plays no part.
        LocalAdjustment adjustment = original;
        adjustment.name.clear();
        adjustment = normalised(std::move(adjustment));

        LocalMaskPlan mask;
        mask.invert = adjustment.invert;
        bool carries = false;
        for (std::size_t row = 0; row < localControlCount; ++row) {
            const LocalDescriptor& descriptor = localAdjustmentDescriptors[row];
            mask.k[row] = adjustment.opacity * adjustment.deltas.*descriptor.member;
            carries = carries || mask.k[row] != 0.0F;
        }
        if (!carries) {
            continue;
        }
        if (const auto* linear = std::get_if<LinearMask>(&adjustment.shape)) {
            resolveLinear(*linear, source, mask);
        } else {
            resolveRadial(std::get<RadialMask>(adjustment.shape), source, mask);
        }
        for (std::size_t row = 0; row < localControlCount; ++row) {
            if (mask.k[row] != 0.0F) {
                plan.touched |= std::uint32_t{1} << row;
            }
        }
        plan.masks.push_back(mask);
    }
    if (plan.masks.empty()) {
        return plan;
    }

    // The globals in setting units, clamped as the global plan clamps them (ADR 008).
    const DevelopSettings& settings = state.settings;
    const ToneAmounts tone = toneAmountsOf(settings.tone);
    const auto control = [&plan](LocalControl which) -> float& {
        return plan.global[indexOf(which)];
    };
    control(LocalControl::Exposure) = tone.exposure;
    control(LocalControl::Contrast) = tone.contrast;
    control(LocalControl::Highlights) = tone.highlights;
    control(LocalControl::Shadows) = tone.shadows;
    control(LocalControl::Whites) = tone.whites;
    control(LocalControl::Blacks) = tone.blacks;
    control(LocalControl::Texture) =
        clampedSetting(settings.presence.texture, weakestPresence, strongestPresence, "texture");
    control(LocalControl::Clarity) =
        clampedSetting(settings.presence.clarity, weakestPresence, strongestPresence, "clarity");
    control(LocalControl::Dehaze) =
        clampedSetting(settings.presence.dehaze, weakestPresence, strongestPresence, "dehaze");
    control(LocalControl::Saturation) = clampedSetting(settings.color.saturation, weakestSaturation,
                                                       strongestSaturation, "saturation");
    control(LocalControl::Vibrance) =
        clampedSetting(settings.color.vibrance, weakestSaturation, strongestSaturation, "vibrance");
    return plan;
}

PresenceReach arraw::presenceReachOf(const LocalPlan& local) noexcept {
    PresenceReach reach;
    const auto add = [&local](PresenceSum& sum, LocalControl control) {
        for (const LocalMaskPlan& mask : local.masks) {
            const float k = mask.k[indexOf(control)];
            sum.negative += std::min(0.0F, k);
            sum.positive += std::max(0.0F, k);
        }
    };
    add(reach.texture, LocalControl::Texture);
    add(reach.clarity, LocalControl::Clarity);
    add(reach.dehaze, LocalControl::Dehaze);
    return reach;
}

PreTapLocal arraw::preTapLocalFieldsOf(const LocalPlan& local) {
    PreTapLocal view;
    for (const LocalMaskPlan& mask : local.masks) {
        const bool carries = std::any_of(mask.k.begin(), mask.k.begin() + preTapControlCount,
                                         [](float k) { return k != 0.0F; });
        if (!carries) {
            continue;
        }
        PreTapMask entry{.kind = mask.kind,
                         .invert = mask.invert,
                         .alpha = mask.alpha,
                         .beta = mask.beta,
                         .gamma = mask.gamma,
                         .centreX = mask.centreX,
                         .centreY = mask.centreY,
                         .matrix = mask.matrix,
                         .inner = mask.inner};
        std::copy_n(mask.k.begin(), preTapControlCount, entry.k.begin());
        view.masks.push_back(entry);
    }
    if (!view.masks.empty()) {
        std::copy_n(local.global.begin(), preTapControlCount, view.global.begin());
    }
    return view;
}
