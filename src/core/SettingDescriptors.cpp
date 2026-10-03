#include "SettingDescriptors.h"

#include <cmath>
#include <format>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace arraw;

namespace {

void check(const FieldDescriptor& descriptor, double value) {
    const SettingRange range = *descriptor.range;
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
    // The curves have no range either: each coordinate lies from 0 to 1, and a
    // curve that breaks its invariants could resolve into no plan.
    for (const ToneCurve* curve : {&settings.toneCurve.luma, &settings.toneCurve.red,
                                   &settings.toneCurve.green, &settings.toneCurve.blue}) {
        if (!isWellFormed(*curve)) {
            throw std::invalid_argument(std::string("a tone curve needs ") + toneCurveRequirements);
        }
    }
    // The crop rows have no range. Whether a crop fits the image is the
    // geometry plan's question, but a crop that could fit no image is refused
    // here, so no photograph holds one.
    const CropSettings& crop = settings.geometry.crop;
    if (crop.rectangle && !isWellFormed(*crop.rectangle)) {
        throw std::invalid_argument("cropRectangle needs finite edges within 0 to 1, left below "
                                    "right and top above bottom");
    }
    if (const auto* ratio = std::get_if<CropRatio>(&crop.aspect); ratio && !isWellFormed(*ratio)) {
        throw std::invalid_argument("cropAspect needs a positive finite ratio");
    }
}
