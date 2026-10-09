#pragma once

#include "ColorSpaces.h"
#include "Presence.h"
#include "TonePlan.h"

#include <ColorEncoding.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <SettingDescriptors.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace arraw {

/// @brief Number of local controls: the rows of ::arraw::localAdjustmentDescriptors.
inline constexpr std::size_t localControlCount = localAdjustmentDescriptors.size();

/// @brief One value per local control, in the table's order (ADR 044, section 2).
using LocalAmounts = std::array<float, localControlCount>;

/// @brief The local controls by their place in the table.
///
/// A static assertion in LocalPlan.cpp ties each name to its row's key.
enum class LocalControl : std::size_t {
    RelativeTemperature, ///< Row 0.
    RelativeTint,        ///< Row 1.
    Exposure,            ///< Row 2.
    Contrast,            ///< Row 3.
    Highlights,          ///< Row 4.
    Shadows,             ///< Row 5.
    Whites,              ///< Row 6.
    Blacks,              ///< Row 7.
    Texture,             ///< Row 8.
    Clarity,             ///< Row 9.
    Dehaze,              ///< Row 10.
    Saturation,          ///< Row 11.
    Vibrance,            ///< Row 12.
};

/// @brief Number of local controls that act before the curve-input tap: the first rows of the
/// table (ADR 044, section 8).
inline constexpr std::size_t preTapControlCount =
    static_cast<std::size_t>(LocalControl::Saturation);

/// @brief Gives a control's index in the table and in a ::arraw::LocalAmounts.
[[nodiscard]] constexpr std::size_t indexOf(LocalControl control) noexcept {
    return static_cast<std::size_t>(control);
}

/// @brief Gives the bit a control has in ::arraw::LocalPlan::touched.
[[nodiscard]] constexpr std::uint32_t bitOf(LocalControl control) noexcept {
    return std::uint32_t{1} << indexOf(control);
}

/// @brief The shape of a mask as the plan holds it.
enum class LocalMaskKind : std::uint32_t {
    Linear = 0, ///< A graduated fade.
    Radial = 1, ///< An oval with a feathered edge.
};

/// @brief One mask, resolved against the size of the source being rendered.
///
/// The plan never holds normalised points (ADR 044, section 4): for a source of a size it holds
/// the coefficients that take a pixel centre `(x + 0.5, y + 0.5)` in source pixels straight to
/// the shape's variable, composed in double and stored as float. A reduced preview and a full
/// export therefore evaluate the same field at their own pixels. The GPU block mirrors this
/// layout field for field.
struct LocalMaskPlan {
    LocalMaskKind kind = LocalMaskKind::Linear; ///< Shape.
    bool invert = false;                        ///< Whether the weight is turned to `1 - w`.

    /// @brief Linear: `t = alpha * px + beta * py + gamma` for a pixel centre `(px, py)`.
    float alpha = 0.0F;
    float beta = 0.0F;  ///< @copydoc alpha
    float gamma = 0.0F; ///< @copydoc alpha

    float centreX = 0.0F; ///< Radial: the centre's column, in source pixels.
    float centreY = 0.0F; ///< Radial: the centre's row, in source pixels.

    /// @brief Radial: row-major 2x2 matrix `M` with `q' = M (px - centreX, py - centreY)` and
    /// `d = |q'|`.
    std::array<float, 4> matrix{};

    /// @brief Radial: the distance `d` below which the weight is one.
    float inner = 0.0F;

    /// @brief What the mask adds to each control where its weight is one: `opacity * delta`,
    /// in setting units, in the table's order.
    LocalAmounts k{};

    friend bool operator==(const LocalMaskPlan&, const LocalMaskPlan&) = default;
};

/// @brief The local adjustments of a plan, resolved: part of the pointwise group (ADR 044).
///
/// Disabled masks, and masks whose every `k` is zero, are not in it, so a photograph whose masks
/// all do nothing has the default block and takes the chain's global path bit for bit. The
/// default block is also the plan of a state without masks, and of ::arraw::planFor overloads
/// that know no size.
struct LocalPlan {
    /// @brief The masks that do something, in the state's order, which is the order they sum in.
    std::vector<LocalMaskPlan> masks;

    /// @brief The global setting of each control in setting units, clamped to the global range;
    /// zero for relative temperature and tint. All zero when there are no masks.
    LocalAmounts global{};

    /// @brief One bit (::arraw::bitOf) per control that any mask in the block carries a non-zero
    /// `k` for, so that a pixel sums only those.
    std::uint32_t touched = 0;

    /// @brief Whether the block holds no mask, and the chain is the global one.
    [[nodiscard]] bool empty() const noexcept {
        return masks.empty();
    }

    friend bool operator==(const LocalPlan&, const LocalPlan&) = default;
};

/// @brief Resolves a state's local adjustments against the size of the source being rendered.
///
/// Every number of every adjustment is normalised first, as an edit would
/// (::arraw::normalised(LocalAdjustment)); the shapes' coefficients are then composed in double.
/// Disabled adjustments and those whose every `opacity * delta` is zero are left out.
/// @param state State to resolve.
/// @param source Size of the pixels the pointwise chain runs on: a pyramid level, a half-size
/// decode, or the full source.
/// @return The block; the default one when nothing remains.
/// @throws std::invalid_argument if a setting or an adjustment's number is not finite, or an
/// adjustment is degenerate.
[[nodiscard]] LocalPlan localPlanFor(const DevelopState& state, ImageSize source);

/// @brief Resolves one mask's shape against the size of the source being rendered.
///
/// The shape's coefficients only: `k` stays zero. What ::arraw::localPlanFor does for each
/// adjustment it keeps, and what ::arraw::maskCoverage does for the one it draws.
/// @param shape Shape, normalised (::arraw::normalised(const Mask&)).
/// @param invert Whether the weight is turned to `1 - w`.
/// @param source Size of the pixels the weight is evaluated at; not empty.
[[nodiscard]] LocalMaskPlan resolvedMask(const Mask& shape, bool invert, ImageSize source);

/// @brief Gives what a block's masks can add to Texture, Clarity and Dehaze, for the bases the
/// Presence plan prepares (ADR 044, section 5).
/// @param local Resolved block.
[[nodiscard]] PresenceReach presenceReachOf(const LocalPlan& local) noexcept;

/// @brief Gives the weight of a mask at a pixel centre, 0 to 1.
///
/// Linear: `1 - smoothstep(0, 1, t)`. Radial: `1 - smoothstep(inner, 1, d)`. Turned to `1 - w`
/// when inverted (ADR 044, section 4).
/// @param mask Resolved mask.
/// @param x Column of the pixel centre in source pixels: the pixel's column plus one half.
/// @param y Row of the pixel centre in source pixels.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline float maskWeight(const LocalMaskPlan& mask, float x, float y) noexcept {
    float weight = 0.0F;
    if (mask.kind == LocalMaskKind::Linear) {
        const float t = mask.alpha * x + mask.beta * y + mask.gamma;
        weight = 1.0F - smoothstep(0.0F, 1.0F, t);
    } else {
        const float dx = x - mask.centreX;
        const float dy = y - mask.centreY;
        const float qx = mask.matrix[0] * dx + mask.matrix[1] * dy;
        const float qy = mask.matrix[2] * dx + mask.matrix[3] * dy;
        const float d = std::sqrt(qx * qx + qy * qy);
        weight = 1.0F - smoothstep(mask.inner, 1.0F, d);
    }
    return mask.invert ? 1.0F - weight : weight;
}

/// @brief Sums every touched control over the masks at a pixel centre, in list order, in float.
///
/// `s_c = sum of w_i * k_i,c` (ADR 044, section 2). Summing before adding the global value
/// makes two masks that cancel exactly cancel to the global value.
/// @param local Resolved block.
/// @param x Column of the pixel centre in source pixels.
/// @param y Row of the pixel centre in source pixels.
/// @return The sums in setting units; zero for a control no mask touches.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline LocalAmounts localSumsAt(const LocalPlan& local, float x, float y) noexcept {
    LocalAmounts sums{};
    for (const LocalMaskPlan& mask : local.masks) {
        const float weight = maskWeight(mask, x, y);
        for (std::size_t control = 0; control < localControlCount; ++control) {
            if ((local.touched >> control & 1U) != 0) {
                sums[control] += weight * mask.k[control];
            }
        }
    }
    return sums;
}

/// @brief Log2 of the red gain of one hundred of relative temperature.
inline constexpr float temperatureRedStops = 0.16F;

/// @brief Log2 of the blue gain of one hundred of relative temperature (cooler is bluer).
inline constexpr float temperatureBlueStops = -0.43F;

/// @brief Log2 of the red gain of one hundred of relative tint.
inline constexpr float tintRedStops = 0.19F;

/// @brief Log2 of the blue gain of one hundred of relative tint (towards magenta).
inline constexpr float tintBlueStops = 0.31F;

/// @brief Resolves relative Temperature and Tint into a diagonal gain in the working space.
///
/// `log2 g_R = 0.16 T/100 + 0.19 N/100`, `log2 g_G = 0`, `log2 g_B = -0.43 T/100 + 0.31 N/100`,
/// then `g` divided by `0.2627 g_R + 0.6780 g_G + 0.0593 g_B`, the working luminance row, so
/// that a neutral keeps the luminance every later stage sees (ADR 044, section 3). Called for
/// non-zero amounts only; zero amounts apply no gain at all.
/// @param temperature Clamped sum of relative temperature, minus a hundred to a hundred.
/// @param tint Clamped sum of relative tint, minus a hundred to a hundred.
/// @return The gain per working channel.
///
/// Mirrored by `src/gpu/shaders/develop.frag`, which must change with it.
[[nodiscard]] inline Colour relativeBalanceGainFor(float temperature, float tint) noexcept {
    constexpr auto limit = static_cast<float>(localControlLimit);
    const float t = temperature / limit;
    const float n = tint / limit;
    const float red = std::exp2(temperatureRedStops * t + tintRedStops * n);
    const float blue = std::exp2(temperatureBlueStops * t + tintBlueStops * n);
    const float norm = colorspaces::workingLuminance[0] * red + colorspaces::workingLuminance[1] +
                       colorspaces::workingLuminance[2] * blue;
    return {red / norm, 1.0F / norm, blue / norm};
}

/// @brief What a mask contributes to the curve-input tap's view of a plan: its shape and the
/// controls that act before the tap.
struct PreTapMask {
    LocalMaskKind kind = LocalMaskKind::Linear; ///< Shape.
    bool invert = false;                        ///< Whether the weight is inverted.
    float alpha = 0.0F;                         ///< @copydoc LocalMaskPlan::alpha
    float beta = 0.0F;                          ///< @copydoc LocalMaskPlan::alpha
    float gamma = 0.0F;                         ///< @copydoc LocalMaskPlan::alpha
    float centreX = 0.0F;                       ///< @copydoc LocalMaskPlan::centreX
    float centreY = 0.0F;                       ///< @copydoc LocalMaskPlan::centreY
    std::array<float, 4> matrix{};              ///< @copydoc LocalMaskPlan::matrix
    float inner = 0.0F;                         ///< @copydoc LocalMaskPlan::inner
    std::array<float, preTapControlCount> k{};  ///< The pre-tap controls' amounts.

    friend bool operator==(const PreTapMask&, const PreTapMask&) = default;
};

/// @brief The part of a local block that the curve-input tap depends on (ADR 044, section 8).
struct PreTapLocal {
    /// @brief The masks that carry a non-zero amount for a pre-tap control, in order.
    std::vector<PreTapMask> masks;

    /// @brief The globals of the pre-tap controls, when there are masks.
    std::array<float, preTapControlCount> global{};

    friend bool operator==(const PreTapLocal&, const PreTapLocal&) = default;
};

/// @brief Gives the local fields the chain reads before the curve-input tap.
///
/// The masks that carry a non-zero `k` for one of the eleven controls before the tap, each with
/// its shape, invert, coefficients and those eleven `k`; and the eleven globals. A Saturation or
/// Vibrance edit, or moving a mask that carries only those, leaves it equal.
/// @param local Resolved block.
[[nodiscard]] PreTapLocal preTapLocalFieldsOf(const LocalPlan& local);

} // namespace arraw
