#include "Edits.h"

#include "SettingCodec.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

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

/// @brief Gives the shape a geometry rule plans against, refusing a photograph without a size.
SourceShape framedShape(const ImageMetadata& photo, std::string_view key) {
    if (photo.size.empty()) {
        throw std::invalid_argument(
            std::format("{} needs the photograph's size, which it does not declare", key));
    }
    return shapeOf(photo);
}

/// @brief Applies a geometry key's rule, leaving the state as it is for an unchanged value.
/// @return Whether @p key names a geometry setting.
bool applyGeometryField(const ImageMetadata& photo, std::string_view key,
                        const GeometrySettings& wanted, GeometrySettings& geometry) {
    if (key == "rotation") {
        if (wanted.rotation != geometry.rotation) {
            geometry = withRotation(std::move(geometry), wanted.rotation);
        }
    } else if (key == "flipHorizontal" || key == "flipVertical") {
        const bool horizontal = key == "flipHorizontal";
        if (wanted.flipHorizontal != geometry.flipHorizontal && horizontal) {
            geometry = flipped(std::move(geometry), true);
        } else if (wanted.flipVertical != geometry.flipVertical && !horizontal) {
            geometry = flipped(std::move(geometry), false);
        }
    } else if (key == "straighten") {
        if (wanted.straighten != geometry.straighten) {
            geometry = rotatedTo(framedShape(photo, key), geometry, wanted.straighten);
        }
    } else if (key == "cropRectangle") {
        if (wanted.crop.rectangle != geometry.crop.rectangle) {
            const SourceShape shape = framedShape(photo, key);
            if (wanted.crop.rectangle) {
                geometry.crop.rectangle = wanted.crop.rectangle;
                geometry.crop.aspect = FreeCropAspect{};
                geometry = fittedCrop(shape, std::move(geometry));
            } else {
                geometry = withCropReset(std::move(geometry));
            }
        }
    } else if (key == "cropAspect") {
        if (wanted.crop.aspect != geometry.crop.aspect) {
            geometry = withAspect(framedShape(photo, key), std::move(geometry), wanted.crop.aspect);
        }
    } else {
        return false;
    }
    return true;
}

} // namespace

DevelopState withField(const ImageMetadata& photo, DevelopState state,
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
    } else if (!applyGeometryField(photo, key, values.geometry, next.geometry)) {
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

DevelopState turned(const ImageMetadata&, DevelopState state, bool clockwise) noexcept {
    state.settings.geometry = turned(std::move(state.settings.geometry), clockwise);
    return state;
}

DevelopState flipped(const ImageMetadata&, DevelopState state, bool horizontal) noexcept {
    state.settings.geometry = flipped(std::move(state.settings.geometry), horizontal);
    return state;
}

DevelopState withAspect(const ImageMetadata& photo, DevelopState state, const CropAspect& aspect) {
    state.settings.geometry =
        withAspect(shapeOf(photo), std::move(state.settings.geometry), aspect);
    return state;
}

DevelopState withLockedAspect(const ImageMetadata& photo, DevelopState state) {
    state.settings.geometry = withLockedAspect(shapeOf(photo), std::move(state.settings.geometry));
    return state;
}

DevelopState withSwappedOrientation(const ImageMetadata& photo, DevelopState state) {
    state.settings.geometry =
        withSwappedOrientation(shapeOf(photo), std::move(state.settings.geometry));
    return state;
}

DevelopState withCropReset(const ImageMetadata&, DevelopState state) noexcept {
    state.settings.geometry = withCropReset(std::move(state.settings.geometry));
    return state;
}

double displayedStraighten(const DevelopState& state) noexcept {
    return displayedStraighten(state.settings.geometry);
}

DevelopState withDisplayedStraighten(const ImageMetadata& photo, DevelopState state,
                                     double displayed) {
    const double stored = storedStraighten(state.settings.geometry, displayed);
    return withValue(photo, std::move(state), "straighten", stored);
}

Look lookOf(const ImageMetadata& photo, const DevelopSettings& settings) {
    return Look{settings, !std::holds_alternative<NamedEncoding>(photo.encoding)};
}

bool sectionApplies(CopySection section, bool raw) noexcept {
    return raw || std::ranges::none_of(developSettingDescriptors, [&](const FieldDescriptor& d) {
               return d.scope == SettingScope::Look && d.section == section &&
                      d.applies == Applicability::RawOnly;
           });
}

AppliedLook withLook(const ImageMetadata& photo, DevelopState state, const Look& look,
                     std::span<const CopySection> sections, const GrainEntropy& entropy) {
    constexpr std::size_t sectionCount = copySectionNames.size();
    std::bitset<sectionCount> chosen;
    for (const CopySection section : sections) {
        const auto index = static_cast<std::size_t>(section);
        if (std::ranges::find(copyableSections, section) == copyableSections.end()) {
            throw std::invalid_argument(std::format(
                "{} is not a section that can be copied",
                index < sectionCount ? copySectionNames[index] : std::string_view{"unknown"}));
        }
        chosen.set(index);
    }

    const bool toRaw = !std::holds_alternative<NamedEncoding>(photo.encoding);
    std::vector<std::string_view> keys;
    std::bitset<sectionCount> lost;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (descriptor.scope != SettingScope::Look || !descriptor.section) {
            continue;
        }
        const auto index = static_cast<std::size_t>(*descriptor.section);
        if (!chosen.test(index)) {
            continue;
        }
        const auto appliesTo = [&](bool raw) {
            return descriptor.applies == Applicability::Always || raw;
        };
        const bool fromSide = appliesTo(look.fromRaw);
        const bool toSide = appliesTo(toRaw);
        if (fromSide && toSide) {
            keys.push_back(descriptor.key);
        } else if (fromSide != toSide) {
            lost.set(index);
        }
    }

    AppliedLook result;
    result.state = withValues(photo, std::move(state), keys, look.settings, entropy);
    for (std::size_t i = 0; i < sectionCount; ++i) {
        if (lost.test(i)) {
            result.skipped.push_back(static_cast<CopySection>(i));
        }
    }
    return result;
}

} // namespace arraw
