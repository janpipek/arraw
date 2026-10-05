#pragma once

#include <SettingDescriptors.h>

#include <cstdint>
#include <optional>
#include <type_traits>

namespace arraw::test {

/// @brief Sets the field a row describes to a valid value that is not its default.
///
/// Ranged numbers get an awkward fraction of their range, so that a round
/// trip through text has something to get wrong.
inline void setNonDefault(const FieldDescriptor& descriptor, DevelopSettings& settings) {
    visitField(descriptor, settings, [&](auto& field) {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double> ||
                      std::is_same_v<T, std::optional<float>>) {
            const SettingRange range = *descriptor.range;
            field = static_cast<std::conditional_t<std::is_same_v<T, double>, double, float>>(
                range.minimum + 0.37 * (range.maximum - range.minimum));
        } else if constexpr (std::is_same_v<T, bool>) {
            field = !field;
        } else if constexpr (std::is_same_v<T, WhiteBalanceMode>) {
            field = WhiteBalanceMode::Custom;
        } else if constexpr (std::is_same_v<T, QuarterTurn>) {
            field = QuarterTurn::Clockwise270;
        } else if constexpr (std::is_same_v<T, std::optional<UprightCropRect>>) {
            field = UprightCropRect{.left = 0.125, .top = 0.1, .right = 0.9, .bottom = 0.85};
        } else if constexpr (std::is_same_v<T, ToneCurve>) {
            field.points = {{0.0F, 0.0625F}, {0.3F, 0.4F}, {0.7F, 0.55F}, {1.0F, 0.96F}};
        } else if constexpr (std::is_same_v<T, GrainModel>) {
            // The one model there is, so the default: nothing else is valid yet.
            field = GrainModel::ValueNoise;
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
            // Above 2^31, and spelled by the shortest decimal as 3e+09.
            field = 3000000000U;
        } else {
            static_assert(std::is_same_v<T, CropAspect>);
            field = CropRatio{1.5};
        }
    });
}

} // namespace arraw::test
