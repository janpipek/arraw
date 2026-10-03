#pragma once

#include <DevelopSettings.h>
#include <RenderCheckpoint.h>
#include <WhiteBalance.h>

#include <array>
#include <optional>
#include <string_view>
#include <variant>

namespace arraw {

/// @brief Accessor to one leaf of ::arraw::DevelopSettings, by the leaf's type.
///
/// The settings are nested, so a row reaches its field through a captureless
/// lambda turned into a function pointer rather than a data-member pointer
/// (ADR 008). Each alternative is the reference-returning accessor for one
/// leaf type.
using SettingAccessor =
    std::variant<float& (*)(DevelopSettings&), std::optional<float>& (*)(DevelopSettings&),
                 double& (*)(DevelopSettings&), bool& (*)(DevelopSettings&),
                 WhiteBalanceMode& (*)(DevelopSettings&), QuarterTurn& (*)(DevelopSettings&),
                 std::optional<UprightCropRect>& (*)(DevelopSettings&),
                 CropAspect& (*)(DevelopSettings&)>;

/// @brief Inclusive numeric limits of a setting, in its own units.
struct SettingRange {
    /// @brief Smallest accepted value.
    double minimum;

    /// @brief Largest accepted value.
    double maximum;
};

/// @brief Panel a setting belongs to.
enum class SettingGroup { Color, Tone, Geometry, Hsl, BlackAndWhite };

/// @brief Whether a setting means anything for every photograph.
enum class Applicability {
    Always,  ///< Applies to any image.
    RawOnly, ///< Needs a sensor to measure against (ADR 008).
};

/// @brief Description of one leaf setting: where it lives, and what it may hold.
struct FieldDescriptor {
    /// @brief camelCase name of the leaf, unique across the table.
    std::string_view key;

    /// @brief Accessor to the leaf inside ::arraw::DevelopSettings.
    SettingAccessor member;

    /// @brief Accepted values, absent for booleans, enumerations and compound rows.
    std::optional<SettingRange> range;

    /// @brief Panel the setting is shown in.
    SettingGroup group;

    /// @brief Photographs the setting applies to.
    Applicability applies;

    /// @brief Earliest pass boundary the setting changes.
    Stage affects;
};

// Local to the table below: a captureless accessor to the leaf `path` of a
// `DevelopSettings s`, undefined again right after it.
#define ARRAW_ACCESSOR(Type, path)                                                                 \
    SettingAccessor {                                                                              \
        +[](DevelopSettings& s) -> Type& { return s.path; }                                        \
    }

// The three rows of one HSL band, and the row of one band's grey weight (ADR 027).
#define ARRAW_HSL_BAND(Name, member)                                                               \
    FieldDescriptor{"hue" #Name,                                                                   \
                    ARRAW_ACCESSOR(float, hsl.member.hue),                                         \
                    SettingRange{weakestHslControl, strongestHslControl},                          \
                    SettingGroup::Hsl,                                                             \
                    Applicability::Always,                                                         \
                    Stage::Pointwise},                                                             \
        FieldDescriptor{"saturation" #Name,                                                        \
                        ARRAW_ACCESSOR(float, hsl.member.saturation),                              \
                        SettingRange{weakestHslControl, strongestHslControl},                      \
                        SettingGroup::Hsl,                                                         \
                        Applicability::Always,                                                     \
                        Stage::Pointwise},                                                         \
        FieldDescriptor {                                                                          \
        "luminance" #Name, ARRAW_ACCESSOR(float, hsl.member.luminance),                            \
            SettingRange{weakestHslControl, strongestHslControl}, SettingGroup::Hsl,               \
            Applicability::Always, Stage::Pointwise                                                \
    }

#define ARRAW_GRAY_BAND(Name, member)                                                              \
    FieldDescriptor {                                                                              \
        "gray" #Name, ARRAW_ACCESSOR(float, blackAndWhite.member),                                 \
            SettingRange{darkestGrayMix, lightestGrayMix}, SettingGroup::BlackAndWhite,            \
            Applicability::Always, Stage::Pointwise                                                \
    }

/// @brief One descriptor per leaf of ::arraw::DevelopSettings.
///
/// Adding a field to a settings struct means adding a row here; a test counts
/// the fields of each struct and fails when the two disagree (ADR 008).
inline constexpr std::array developSettingDescriptors{
    FieldDescriptor{"exposure", ARRAW_ACCESSOR(float, tone.exposure),
                    SettingRange{darkestExposure, brightestExposure}, SettingGroup::Tone,
                    Applicability::Always, Stage::Pointwise},
    FieldDescriptor{"contrast", ARRAW_ACCESSOR(float, tone.contrast),
                    SettingRange{flattestContrast, steepestContrast}, SettingGroup::Tone,
                    Applicability::Always, Stage::Pointwise},
    FieldDescriptor{"shadows", ARRAW_ACCESSOR(float, tone.shadows),
                    SettingRange{weakestToneControl, strongestToneControl}, SettingGroup::Tone,
                    Applicability::Always, Stage::Pointwise},
    FieldDescriptor{"highlights", ARRAW_ACCESSOR(float, tone.highlights),
                    SettingRange{weakestToneControl, strongestToneControl}, SettingGroup::Tone,
                    Applicability::Always, Stage::Pointwise},
    FieldDescriptor{"blacks", ARRAW_ACCESSOR(float, tone.blacks),
                    SettingRange{weakestToneControl, strongestToneControl}, SettingGroup::Tone,
                    Applicability::Always, Stage::Pointwise},
    FieldDescriptor{"whites", ARRAW_ACCESSOR(float, tone.whites),
                    SettingRange{weakestToneControl, strongestToneControl}, SettingGroup::Tone,
                    Applicability::Always, Stage::Pointwise},
    FieldDescriptor{"filmicHighlights", ARRAW_ACCESSOR(float, tone.filmicHighlights),
                    SettingRange{noFilmicHighlights, fullFilmicHighlights}, SettingGroup::Tone,
                    Applicability::Always, Stage::Pointwise},
    FieldDescriptor{"whiteBalance", ARRAW_ACCESSOR(WhiteBalanceMode, color.whiteBalance),
                    std::nullopt, SettingGroup::Color, Applicability::Always, Stage::Pointwise},
    FieldDescriptor{"temperature", ARRAW_ACCESSOR(std::optional<float>, color.temperature),
                    SettingRange{warmestKelvin, coolestKelvin}, SettingGroup::Color,
                    Applicability::RawOnly, Stage::Pointwise},
    FieldDescriptor{"tint", ARRAW_ACCESSOR(std::optional<float>, color.tint),
                    SettingRange{-tintLimit, tintLimit}, SettingGroup::Color,
                    Applicability::RawOnly, Stage::Pointwise},
    FieldDescriptor{"saturation", ARRAW_ACCESSOR(float, color.saturation),
                    SettingRange{weakestSaturation, strongestSaturation}, SettingGroup::Color,
                    Applicability::Always, Stage::Pointwise},
    FieldDescriptor{"vibrance", ARRAW_ACCESSOR(float, color.vibrance),
                    SettingRange{weakestSaturation, strongestSaturation}, SettingGroup::Color,
                    Applicability::Always, Stage::Pointwise},
    ARRAW_HSL_BAND(Red, red),
    ARRAW_HSL_BAND(Orange, orange),
    ARRAW_HSL_BAND(Yellow, yellow),
    ARRAW_HSL_BAND(Green, green),
    ARRAW_HSL_BAND(Aqua, aqua),
    ARRAW_HSL_BAND(Blue, blue),
    ARRAW_HSL_BAND(Purple, purple),
    ARRAW_HSL_BAND(Magenta, magenta),
    FieldDescriptor{"convertToGrayscale", ARRAW_ACCESSOR(bool, blackAndWhite.convertToGrayscale),
                    std::nullopt, SettingGroup::BlackAndWhite, Applicability::Always,
                    Stage::Pointwise},
    ARRAW_GRAY_BAND(Red, red),
    ARRAW_GRAY_BAND(Orange, orange),
    ARRAW_GRAY_BAND(Yellow, yellow),
    ARRAW_GRAY_BAND(Green, green),
    ARRAW_GRAY_BAND(Aqua, aqua),
    ARRAW_GRAY_BAND(Blue, blue),
    ARRAW_GRAY_BAND(Purple, purple),
    ARRAW_GRAY_BAND(Magenta, magenta),
    FieldDescriptor{"rotation", ARRAW_ACCESSOR(QuarterTurn, geometry.rotation), std::nullopt,
                    SettingGroup::Geometry, Applicability::Always, Stage::Geometry},
    FieldDescriptor{"flipHorizontal", ARRAW_ACCESSOR(bool, geometry.flipHorizontal), std::nullopt,
                    SettingGroup::Geometry, Applicability::Always, Stage::Geometry},
    FieldDescriptor{"flipVertical", ARRAW_ACCESSOR(bool, geometry.flipVertical), std::nullopt,
                    SettingGroup::Geometry, Applicability::Always, Stage::Geometry},
    FieldDescriptor{"straighten", ARRAW_ACCESSOR(double, geometry.straighten),
                    SettingRange{minimumStraighten, maximumStraighten}, SettingGroup::Geometry,
                    Applicability::Always, Stage::Geometry},
    FieldDescriptor{"cropRectangle",
                    ARRAW_ACCESSOR(std::optional<UprightCropRect>, geometry.crop.rectangle),
                    std::nullopt, SettingGroup::Geometry, Applicability::Always, Stage::Geometry},
    FieldDescriptor{"cropAspect", ARRAW_ACCESSOR(CropAspect, geometry.crop.aspect), std::nullopt,
                    SettingGroup::Geometry, Applicability::Always, Stage::Geometry},
};

#undef ARRAW_GRAY_BAND
#undef ARRAW_HSL_BAND
#undef ARRAW_ACCESSOR

/// @brief Finds the descriptor with a given key.
/// @param key camelCase leaf name, such as "exposure".
/// @return The row, or null when no setting has that key.
[[nodiscard]] constexpr const FieldDescriptor* findDescriptor(std::string_view key) noexcept {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (descriptor.key == key) {
            return &descriptor;
        }
    }
    return nullptr;
}

/// @brief Calls a visitor with a mutable reference to the field a row describes.
/// @param descriptor Row naming the field.
/// @param settings Settings holding the field.
/// @param visitor Callable taking `T&` for every leaf type T in ::arraw::SettingAccessor.
/// @return Whatever the visitor returns.
template <class Visitor>
decltype(auto) visitField(const FieldDescriptor& descriptor, DevelopSettings& settings,
                          Visitor&& visitor) {
    return std::visit([&](auto accessor) -> decltype(auto) { return visitor(accessor(settings)); },
                      descriptor.member);
}

/// @brief Calls a visitor with a read-only reference to the field a row describes.
///
/// The accessors take a mutable settings object, so this is the one place that
/// casts constness away; the visitor only ever sees the field as `const T&`,
/// which keeps `settings` unmodified.
/// @param descriptor Row naming the field.
/// @param settings Settings holding the field.
/// @param visitor Callable taking `const T&` for every leaf type T in ::arraw::SettingAccessor.
/// @return Whatever the visitor returns.
template <class Visitor>
decltype(auto) visitField(const FieldDescriptor& descriptor, const DevelopSettings& settings,
                          Visitor&& visitor) {
    return std::visit(
        [&](auto accessor) -> decltype(auto) {
            const auto& field = accessor(const_cast<DevelopSettings&>(settings));
            return visitor(field);
        },
        descriptor.member);
}

/// @brief Checks every ranged setting against its descriptor.
///
/// Non-finite values are refused whatever the range, and optional values are
/// checked only when set. Enumerations, booleans and the crop rows have no
/// range here: their constraints live with the geometry plan (ADR 014).
/// @param settings Settings to check.
/// @throws std::invalid_argument naming the key, the value and the range of
/// the first setting that is not finite or lies outside its range.
void validate(const DevelopSettings& settings);

} // namespace arraw
