#include "SettingDescriptors.h"

#include <cmath>
#include <format>
#include <stdexcept>
#include <type_traits>

using namespace arraw;

namespace {

void check(const FieldDescriptor& descriptor, double value) {
    const Range range = *descriptor.range;
    if (!std::isfinite(value) || value < range.minimum || value > range.maximum) {
        throw std::invalid_argument(std::format("{} is {}, outside its range {} to {}",
                                                descriptor.key, value, range.minimum,
                                                range.maximum));
    }
}

} // namespace

void arraw::validate(const DevelopSettings& settings) {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (!descriptor.range) {
            continue;
        }
        visitField(descriptor, settings, [&](const auto& field) {
            using T = std::remove_cvref_t<decltype(field)>;
            if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
                check(descriptor, field);
            } else if constexpr (std::is_same_v<T, std::optional<float>>) {
                if (field) {
                    check(descriptor, *field);
                }
            }
        });
    }
    // The crop rows have no range: GeometryPlan owns their geometry, but a
    // non-finite value is refused here so no photograph holds one.
    const CropSettings& crop = settings.geometry.crop;
    if (crop.rectangle) {
        for (const double edge : {crop.rectangle->left, crop.rectangle->top, crop.rectangle->right,
                                  crop.rectangle->bottom}) {
            if (!std::isfinite(edge)) {
                throw std::invalid_argument("cropRectangle is not finite");
            }
        }
    }
    if (const auto* ratio = std::get_if<CropRatio>(&crop.aspect);
        ratio && !std::isfinite(ratio->widthOverHeight)) {
        throw std::invalid_argument("cropAspect is not finite");
    }
}
