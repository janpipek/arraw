#include "LocalAdjustments.h"

#include <SettingDescriptors.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace arraw;

namespace {

/// @brief Refuses a number that is not finite.
void requireFinite(float value, std::string_view what) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::format("{} is not a finite number", what));
    }
}

/// @brief Refuses a number outside a range, or one that is not finite.
void requireWithin(float value, double low, double high, std::string_view what) {
    requireFinite(value, what);
    if (value < low || value > high) {
        throw std::invalid_argument(
            std::format("{} is {}, outside its range {} to {}", what, value, low, high));
    }
}

/// @brief Distance between two normalised points.
double distanceBetween(CorrectedPoint a, CorrectedPoint b) noexcept {
    return std::hypot(static_cast<double>(b.u) - a.u, static_cast<double>(b.v) - a.v);
}

/// @brief Clamps a point, refusing one that is not finite.
CorrectedPoint clampedPoint(CorrectedPoint point, std::string_view what) {
    requireFinite(point.u, what);
    requireFinite(point.v, what);
    return {std::clamp(point.u, minimumMaskPosition, maximumMaskPosition),
            std::clamp(point.v, minimumMaskPosition, maximumMaskPosition)};
}

/// @brief Refuses a point outside the positions a handle may take.
void requirePoint(CorrectedPoint point, std::string_view what) {
    requireWithin(point.u, minimumMaskPosition, maximumMaskPosition, what);
    requireWithin(point.v, minimumMaskPosition, maximumMaskPosition, what);
}

/// @brief Reads one code point from the front of a UTF-8 text.
/// @param text Text to read from; consumed by the bytes of the code point, or by one byte if the
/// sequence is not valid UTF-8 (overlong, a surrogate, beyond U+10FFFF or truncated).
/// @param valid Set to whether a valid code point was read.
/// @return The code point, or 0 if not valid.
char32_t nextCodePoint(std::string_view& text, bool& valid) noexcept {
    const auto lead = static_cast<unsigned char>(text.front());
    int extra = 0;
    char32_t code = lead;
    char32_t least = 0;
    if (lead < 0x80) {
        extra = 0;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
        extra = 1;
        code = lead & 0x1FU;
        least = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        extra = 2;
        code = lead & 0x0FU;
        least = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        extra = 3;
        code = lead & 0x07U;
        least = 0x10000;
    } else {
        valid = false;
        text.remove_prefix(1);
        return 0;
    }
    if (text.size() <= static_cast<std::size_t>(extra)) {
        valid = extra == 0;
        text.remove_prefix(extra == 0 ? 1 : text.size());
        return extra == 0 ? code : 0;
    }
    for (int i = 1; i <= extra; ++i) {
        const auto next = static_cast<unsigned char>(text[static_cast<std::size_t>(i)]);
        if ((next & 0xC0U) != 0x80U) {
            valid = false;
            text.remove_prefix(static_cast<std::size_t>(i));
            return 0;
        }
        code = (code << 6) | (next & 0x3FU);
    }
    text.remove_prefix(static_cast<std::size_t>(extra) + 1);
    valid = code >= least && code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF);
    return valid ? code : 0;
}

/// @brief Whether a code point may be in a mask's name: one XML 1.0 can carry, and not a control.
bool isNameCodePoint(char32_t code) noexcept {
    return code >= 0x20 && code != 0x7F &&
           (code <= 0xD7FF || (code >= 0xE000 && code <= 0xFFFD) ||
            (code >= 0x10000 && code <= 0x10FFFF));
}

/// @brief Whether a name is valid UTF-8 made only of storable characters.
bool isStorableName(std::string_view name) noexcept {
    while (!name.empty()) {
        bool valid = false;
        const char32_t code = nextCodePoint(name, valid);
        if (!valid || !isNameCodePoint(code)) {
            return false;
        }
    }
    return true;
}

/// @brief Refuses a linear mask's ends that are too close.
void refuseDegenerate(double distance) {
    if (distance < minimumMaskExtent) {
        throw std::invalid_argument(
            std::format("a linear mask's ends are {} apart, closer than the {} a mask needs",
                        distance, minimumMaskExtent));
    }
}

/// @brief Refuses a radius below the smallest extent.
void refuseThinRadius(float radius, std::string_view what) {
    if (radius < minimumMaskExtent) {
        throw std::invalid_argument(
            std::format("{} is {}, below the {} a mask needs", what, radius, minimumMaskExtent));
    }
}

} // namespace

std::string arraw::storableMaskName(std::string_view name) {
    std::string clean;
    while (!name.empty()) {
        const std::string_view before = name;
        bool valid = false;
        const char32_t code = nextCodePoint(name, valid);
        if (valid && isNameCodePoint(code)) {
            clean.append(before.substr(0, before.size() - name.size()));
        }
    }
    return clean;
}

std::string_view arraw::maskTypeName(const Mask& shape) noexcept {
    switch (shape.index()) {
    case 0:
        return "linear";
    case 1:
        return "radial";
    case 2:
        return "brush";
    default:
        break;
    }
    return "unknown";
}

float arraw::wrappedAngle(float degrees) noexcept {
    double wrapped = std::fmod(static_cast<double>(degrees) + 180.0, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return static_cast<float>(wrapped - 180.0);
}

Mask arraw::normalised(const Mask& shape) {
    return std::visit(
        [](const auto& mask) -> Mask {
            using T = std::remove_cvref_t<decltype(mask)>;
            if constexpr (std::is_same_v<T, BrushMask>) {
                BrushMask checked = mask;
                if (checked.strokes == nullptr) {
                    checked.strokes = emptyStrokeList();
                } else if (checked.strokes->rasteriser() != brushRasteriserVersion) {
                    throw std::invalid_argument(
                        std::format("a brush mask's rasteriser {} is not the supported {}",
                                    checked.strokes->rasteriser(), brushRasteriserVersion));
                }
                return checked;
            } else if constexpr (std::is_same_v<T, LinearMask>) {
                const LinearMask clamped{clampedPoint(mask.from, "a linear mask's from point"),
                                         clampedPoint(mask.to, "a linear mask's to point")};
                refuseDegenerate(distanceBetween(clamped.from, clamped.to));
                return clamped;
            } else {
                RadialMask clamped = mask;
                clamped.centre = clampedPoint(mask.centre, "a radial mask's centre");
                requireFinite(mask.radiusX, "a radial mask's radiusX");
                requireFinite(mask.radiusY, "a radial mask's radiusY");
                requireFinite(mask.angle, "a radial mask's angle");
                requireFinite(mask.feather, "a radial mask's feather");
                refuseThinRadius(mask.radiusX, "a radial mask's radiusX");
                refuseThinRadius(mask.radiusY, "a radial mask's radiusY");
                clamped.radiusX = std::min(mask.radiusX, maximumMaskRadius);
                clamped.radiusY = std::min(mask.radiusY, maximumMaskRadius);
                clamped.angle = wrappedAngle(mask.angle);
                clamped.feather = std::clamp(mask.feather, 0.0F, 1.0F);
                return clamped;
            }
        },
        shape);
}

LocalAdjustment arraw::normalised(LocalAdjustment adjustment) {
    if (!isStorableName(adjustment.name)) {
        throw std::invalid_argument("a mask's name must be valid UTF-8 without control characters "
                                    "or characters XML cannot hold");
    }
    requireFinite(adjustment.opacity, "opacity");
    adjustment.opacity = std::clamp(adjustment.opacity, 0.0F, 1.0F);
    adjustment.shape = normalised(adjustment.shape);
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        float& delta = adjustment.deltas.*descriptor.member;
        requireFinite(delta, descriptor.key);
        delta = static_cast<float>(std::clamp(static_cast<double>(delta), descriptor.range.minimum,
                                              descriptor.range.maximum));
    }
    return adjustment;
}

void arraw::validate(const LocalAdjustment& adjustment) {
    if (!isStorableName(adjustment.name)) {
        throw std::invalid_argument("a mask's name must be valid UTF-8 without control characters "
                                    "or characters XML cannot hold");
    }
    requireWithin(adjustment.opacity, 0.0, 1.0, "opacity");
    for (const LocalDescriptor& descriptor : localAdjustmentDescriptors) {
        requireWithin(adjustment.deltas.*descriptor.member, descriptor.range.minimum,
                      descriptor.range.maximum, descriptor.key);
    }
    std::visit(
        [](const auto& mask) {
            using T = std::remove_cvref_t<decltype(mask)>;
            if constexpr (std::is_same_v<T, BrushMask>) {
                if (mask.strokes == nullptr) {
                    throw std::invalid_argument("a brush mask has no stroke list");
                }
                if (mask.strokes->rasteriser() != brushRasteriserVersion) {
                    throw std::invalid_argument(
                        std::format("a brush mask's rasteriser {} is not the supported {}",
                                    mask.strokes->rasteriser(), brushRasteriserVersion));
                }
            } else if constexpr (std::is_same_v<T, LinearMask>) {
                requirePoint(mask.from, "a linear mask's from point");
                requirePoint(mask.to, "a linear mask's to point");
                refuseDegenerate(distanceBetween(mask.from, mask.to));
            } else {
                requirePoint(mask.centre, "a radial mask's centre");
                requireWithin(mask.radiusX, minimumMaskExtent, maximumMaskRadius,
                              "a radial mask's radiusX");
                requireWithin(mask.radiusY, minimumMaskExtent, maximumMaskRadius,
                              "a radial mask's radiusY");
                requireWithin(mask.angle, -180.0, std::nextafter(180.0F, 0.0F),
                              "a radial mask's angle");
                requireWithin(mask.feather, 0.0, 1.0, "a radial mask's feather");
            }
        },
        adjustment.shape);
}
