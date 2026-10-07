#include "Edits.h"

#include "SettingCodec.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace arraw {

namespace detail {

const FieldDescriptor& editedDescriptor(std::string_view key) {
    const FieldDescriptor* descriptor = findDescriptor(key);
    if (descriptor == nullptr) {
        throw std::invalid_argument(std::format("no setting is named \"{}\"", key));
    }
    return *descriptor;
}

void checkNumber(const FieldDescriptor& descriptor, double value) {
    const SettingRange range = *descriptor.range;
    if (!std::isfinite(value) || value < range.minimum || value > range.maximum) {
        throw std::invalid_argument(std::format("{} is {}, outside its range {} to {}",
                                                descriptor.key, value, range.minimum,
                                                range.maximum));
    }
}

void refuseType(const FieldDescriptor& descriptor) {
    throw std::invalid_argument(
        std::format("{} takes {}", descriptor.key, expectation(descriptor)));
}

namespace {

/// @brief Checks the one field a row describes, as ::arraw::validate checks it.
void validateField(const FieldDescriptor& descriptor, const DevelopSettings& values) {
    visitField(descriptor, values, [&](const auto& field) {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
            checkNumber(descriptor, field);
        } else if constexpr (std::is_same_v<T, std::optional<float>>) {
            if (field) {
                checkNumber(descriptor, *field);
            }
        } else if constexpr (std::is_same_v<T, ToneCurve>) {
            if (!isWellFormed(field)) {
                throw std::invalid_argument(std::string("a tone curve needs ") +
                                            toneCurveRequirements);
            }
        } else if constexpr (std::is_same_v<T, std::optional<UprightCropRect>>) {
            if (field && !isWellFormed(*field)) {
                throw std::invalid_argument("cropRectangle needs finite edges within 0 to 1, "
                                            "left below right and top above bottom");
            }
        } else if constexpr (std::is_same_v<T, CropAspect>) {
            if (const auto* ratio = std::get_if<CropRatio>(&field);
                ratio && !isWellFormed(*ratio)) {
                throw std::invalid_argument("cropAspect needs a positive finite ratio");
            }
        }
    });
}

/// @brief Copies the field a row describes from one set of settings to another.
void copyField(const FieldDescriptor& descriptor, const DevelopSettings& from,
               DevelopSettings& to) {
    visitField(descriptor, from, [&](const auto& source) {
        visitField(descriptor, to, [&](auto& target) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(source)>,
                                         std::remove_cvref_t<decltype(target)>>) {
                target = source;
            }
        });
    });
}

void clearLight(ColorSettings& color) {
    color.temperature.reset();
    color.tint.reset();
}

} // namespace

DevelopState withField(const ImageMetadata& photo [[maybe_unused]], DevelopState state,
                       const FieldDescriptor& descriptor, const DevelopSettings& values,
                       const GrainEntropy& entropy) {
    validateField(descriptor, values);

    const DevelopSettings& before = state.settings;
    DevelopSettings next = before;
    const std::string_view key = descriptor.key;

    if (key == "temperature" || key == "tint") {
        // In As Shot the rows showed the camera's reading, so a leftover in the other half is
        // not adopted.
        if (before.color.whiteBalance == WhiteBalanceMode::AsShot) {
            clearLight(next.color);
        }
        copyField(descriptor, values, next);
        next.color.whiteBalance = next.color.temperature || next.color.tint
                                      ? WhiteBalanceMode::Custom
                                      : WhiteBalanceMode::AsShot;
    } else if (key == "whiteBalance") {
        const WhiteBalanceMode mode = values.color.whiteBalance;
        if (mode == WhiteBalanceMode::AsShot ||
            before.color.whiteBalance == WhiteBalanceMode::AsShot) {
            clearLight(next.color);
        }
        next.color.whiteBalance = mode;
    } else if (key == "grainAmount") {
        copyField(descriptor, values, next);
        next.effects.grain.seed =
            chooseGrainSeed(before.effects.grain, next.effects.grain, entropy);
    } else {
        // Geometry included: plain assignment until the geometry rules move into core.
        copyField(descriptor, values, next);
    }
    state.settings = std::move(next);
    return state;
}

} // namespace detail

DevelopState withValueFrom(const ImageMetadata& photo, DevelopState state, std::string_view key,
                           const DevelopSettings& source, const GrainEntropy& entropy) {
    const FieldDescriptor& descriptor = detail::editedDescriptor(key);
    DevelopSettings values = source;
    if ((key == "temperature" || key == "tint") &&
        source.color.whiteBalance == WhiteBalanceMode::AsShot) {
        detail::clearLight(values.color);
    }
    return detail::withField(photo, std::move(state), descriptor, values, entropy);
}

DevelopState withValues(const ImageMetadata& photo, DevelopState state,
                        std::span<const std::string_view> keys, const DevelopSettings& source,
                        const GrainEntropy& entropy) {
    std::bitset<developSettingDescriptors.size()> wanted;
    for (const std::string_view key : keys) {
        wanted.set(static_cast<std::size_t>(&detail::editedDescriptor(key) -
                                            developSettingDescriptors.data()));
    }
    for (std::size_t i = 0; i < developSettingDescriptors.size(); ++i) {
        if (wanted.test(i)) {
            state = withValueFrom(photo, std::move(state), developSettingDescriptors[i].key, source,
                                  entropy);
        }
    }
    return state;
}

} // namespace arraw
