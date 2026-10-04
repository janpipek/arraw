#include "PyBindings.h"

#include <ColorGradingSettings.h>
#include <DevelopSettings.h>
#include <SettingDescriptors.h>
#include <SettingsJson.h>
#include <ToneCurveSettings.h>

#include <cctype>
#include <string>
#include <vector>

namespace arraw::python {

namespace {

/// @brief Python-side view of one row of the descriptor table.
struct SettingDescriptor {
    /// @brief camelCase key, as in the C++ table.
    std::string key;

    /// @brief snake_case keyword, derived from the key.
    std::string name;

    /// @brief Inclusive limits, absent for booleans, enumerations and compound rows.
    std::optional<std::pair<double, double>> range;

    /// @brief Panel the setting belongs to.
    SettingGroup group;

    /// @brief Photographs the setting applies to.
    Applicability applies;

    /// @brief Earliest pass boundary the setting changes.
    Stage affects;

    friend bool operator==(const SettingDescriptor&, const SettingDescriptor&) = default;
};

/// @brief Converts a camelCase key to snake_case, mechanically.
/// @param key Descriptor key such as "filmicHighlights".
/// @return The keyword, such as "filmic_highlights".
std::string snakeCase(std::string_view key) {
    std::string result;
    for (const char c : key) {
        if (std::isupper(static_cast<unsigned char>(c)) != 0) {
            result += '_';
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            result += c;
        }
    }
    return result;
}

/// @brief Lists the descriptor table as Python objects.
std::vector<SettingDescriptor> listDescriptors() {
    std::vector<SettingDescriptor> result;
    for (const FieldDescriptor& row : developSettingDescriptors) {
        std::optional<std::pair<double, double>> range;
        if (row.range) {
            range = std::pair{row.range->minimum, row.range->maximum};
        }
        result.push_back(
            {std::string(row.key), snakeCase(row.key), range, row.group, row.applies, row.affects});
    }
    return result;
}

} // namespace

void applyFlatSettings(DevelopSettings& settings, const nb::kwargs& keywords) {
    for (auto [key, value] : keywords) {
        const std::string name = nb::cast<std::string>(key);
        const FieldDescriptor* match = nullptr;
        for (const FieldDescriptor& row : developSettingDescriptors) {
            if (snakeCase(row.key) == name) {
                match = &row;
                break;
            }
        }
        if (match == nullptr) {
            throw nb::type_error(("unknown develop setting '" + name + "'").c_str());
        }
        visitField(*match, settings, [&](auto& leaf) {
            using Leaf = std::remove_cvref_t<decltype(leaf)>;
            if constexpr (std::is_same_v<Leaf, ToneCurve>) {
                // A curve may be given as a ToneCurve or as its list of (x, y) points.
                if (!nb::isinstance<ToneCurve>(value)) {
                    leaf = ToneCurve{convertValue<std::vector<CurvePoint>>(value, name)};
                    normaliseCurvePoints(leaf.points);
                    return;
                }
            }
            leaf = convertValue<Leaf>(value, name);
        });
    }
}

void bindSettings(nb::module_& m) {
    nb::enum_<WhiteBalanceMode>(m, "WhiteBalanceMode",
                                "Where a photograph's white balance comes from.")
        .value("AS_SHOT", WhiteBalanceMode::AsShot)
        .value("CUSTOM", WhiteBalanceMode::Custom);

    nb::enum_<QuarterTurn>(m, "QuarterTurn", "User rotation clockwise, in exact quarter-turns.")
        .value("NONE", QuarterTurn::None)
        .value("CLOCKWISE_90", QuarterTurn::Clockwise90)
        .value("CLOCKWISE_180", QuarterTurn::Clockwise180)
        .value("CLOCKWISE_270", QuarterTurn::Clockwise270);

    nb::enum_<SettingGroup>(m, "SettingGroup", "Panel a setting belongs to.")
        .value("COLOR", SettingGroup::Color)
        .value("TONE", SettingGroup::Tone)
        .value("GEOMETRY", SettingGroup::Geometry)
        .value("HSL", SettingGroup::Hsl)
        .value("BLACK_AND_WHITE", SettingGroup::BlackAndWhite)
        .value("TONE_CURVE", SettingGroup::ToneCurve)
        .value("COLOR_GRADING", SettingGroup::ColorGrading);

    nb::enum_<Applicability>(m, "Applicability",
                             "Whether a setting means anything for every photograph.")
        .value("ALWAYS", Applicability::Always)
        .value("RAW_ONLY", Applicability::RawOnly);

    nb::enum_<Stage>(m, "Stage", "Pass boundary of the render pipeline.")
        .value("POINTWISE", Stage::Pointwise)
        .value("GEOMETRY", Stage::Geometry)
        .value("RESIZE", Stage::Resize);

    bindFrozen<ToneSettings>(
        m, "ToneSettings", "Photographic tone adjustments.",
        field("exposure", &ToneSettings::exposure), field("contrast", &ToneSettings::contrast),
        field("shadows", &ToneSettings::shadows), field("highlights", &ToneSettings::highlights),
        field("blacks", &ToneSettings::blacks), field("whites", &ToneSettings::whites),
        field("filmic_highlights", &ToneSettings::filmicHighlights));

    bindFrozen<ColorSettings>(m, "ColorSettings", "Photographic colour adjustments.",
                              field("white_balance", &ColorSettings::whiteBalance),
                              field("temperature", &ColorSettings::temperature),
                              field("tint", &ColorSettings::tint),
                              field("saturation", &ColorSettings::saturation),
                              field("vibrance", &ColorSettings::vibrance));

    bindFrozen<HueBand>(m, "HueBand", "Hue, saturation and luminance shifts of one band of hues.",
                        field("hue", &HueBand::hue), field("saturation", &HueBand::saturation),
                        field("luminance", &HueBand::luminance));
    bindFrozen<HslSettings>(
        m, "HslSettings", "Per-hue colour adjustments over eight bands.",
        field("red", &HslSettings::red), field("orange", &HslSettings::orange),
        field("yellow", &HslSettings::yellow), field("green", &HslSettings::green),
        field("aqua", &HslSettings::aqua), field("blue", &HslSettings::blue),
        field("purple", &HslSettings::purple), field("magenta", &HslSettings::magenta));
    bindFrozen<BlackAndWhiteSettings>(
        m, "BlackAndWhiteSettings", "Conversion to grey and the mix of hues it is made from.",
        field("convert_to_grayscale", &BlackAndWhiteSettings::convertToGrayscale),
        field("red", &BlackAndWhiteSettings::red), field("orange", &BlackAndWhiteSettings::orange),
        field("yellow", &BlackAndWhiteSettings::yellow),
        field("green", &BlackAndWhiteSettings::green), field("aqua", &BlackAndWhiteSettings::aqua),
        field("blue", &BlackAndWhiteSettings::blue),
        field("purple", &BlackAndWhiteSettings::purple),
        field("magenta", &BlackAndWhiteSettings::magenta));

    bindFrozen<FreeCropAspect>(m, "FreeCropAspect", "Unconstrained crop aspect.");
    bindFrozen<OriginalCropAspect>(m, "OriginalCropAspect",
                                   "Original image aspect after orientation and quarter-turns.");
    bindFrozen<CropRatio, false>(m, "CropRatio", "Fixed width-to-height crop ratio.",
                                 field("width_over_height", &CropRatio::widthOverHeight));
    bindFrozen<UprightCropRect, false>(
        m, "UprightCropRect", "Crop edges normalised to the uncropped upright rectangle.",
        field("left", &UprightCropRect::left), field("top", &UprightCropRect::top),
        field("right", &UprightCropRect::right), field("bottom", &UprightCropRect::bottom));
    bindFrozen<CropSettings>(m, "CropSettings", "Framing and its remembered aspect constraint.",
                             field("rectangle", &CropSettings::rectangle),
                             field("aspect", &CropSettings::aspect));
    bindFrozen<GeometrySettings>(m, "GeometrySettings", "Orientation, straightening and crop.",
                                 field("rotation", &GeometrySettings::rotation),
                                 field("flip_horizontal", &GeometrySettings::flipHorizontal),
                                 field("flip_vertical", &GeometrySettings::flipVertical),
                                 field("straighten", &GeometrySettings::straighten),
                                 field("crop", &GeometrySettings::crop));

    bindFrozen<ToneCurve, false>(
        m, "ToneCurve",
        "A tone curve as 2 to 16 (x, y) control points from x = 0 to x = 1, x at least 0.01 "
        "apart, given in any order and sorted by x; the default is the identity.",
        field("points", &ToneCurve::points))
        .def_prop_ro("is_identity", &ToneCurve::isIdentity,
                     "Whether the curve is exactly the line from (0, 0) to (1, 1).");
    bindFrozen<ToneCurveSettings>(
        m, "ToneCurveSettings", "Tone curves on luminance and on the red, green and blue channels.",
        field("luma", &ToneCurveSettings::luma), field("red", &ToneCurveSettings::red),
        field("green", &ToneCurveSettings::green), field("blue", &ToneCurveSettings::blue));

    bindFrozen<GradeZone>(
        m, "GradeZone",
        "Tint of one tonal zone: a hue and how much of it.\n\n"
        "The hue is an Oklab hue angle in degrees, not Lightroom's: roughly 30 is "
        "red, 110 yellow, 140 green and 260 blue.",
        field("hue", &GradeZone::hue), field("saturation", &GradeZone::saturation));
    bindFrozen<ColorGradingSettings>(m, "ColorGradingSettings",
                                     "Three-zone toning of the shadows, midtones and highlights.",
                                     field("shadows", &ColorGradingSettings::shadows),
                                     field("midtones", &ColorGradingSettings::midtones),
                                     field("highlights", &ColorGradingSettings::highlights),
                                     field("balance", &ColorGradingSettings::balance),
                                     field("blending", &ColorGradingSettings::blending));

    bindFrozen<DevelopSettings>(
        m, "DevelopSettings", "Photographic settings of one photograph.",
        field("color", &DevelopSettings::color), field("geometry", &DevelopSettings::geometry),
        field("tone", &DevelopSettings::tone), field("hsl", &DevelopSettings::hsl),
        field("black_and_white", &DevelopSettings::blackAndWhite),
        field("tone_curve", &DevelopSettings::toneCurve),
        field("color_grading", &DevelopSettings::colorGrading))
        .def(
            "with_",
            [](const DevelopSettings& self, const nb::kwargs& keywords) {
                DevelopSettings copy = self;
                applyFlatSettings(copy, keywords);
                return copy;
            },
            "Return a copy with flat snake_case keywords applied, e.g. exposure=0.7.")
        .def(
            "to_json", [](const DevelopSettings& self) { return settingsToJson(self); },
            "Write the settings as a JSON document.")
        .def_static(
            "from_json",
            [](const std::string& text, const std::optional<DevelopSettings>& base) {
                PythonLog log;
                return applySettingsJson(text, base.value_or(DevelopSettings{}), log);
            },
            "text"_a, "base"_a = nb::none(),
            "Read a JSON document onto `base` (the defaults when None). Keys that are absent "
            "keep the base's value; problems with single settings are logged as warnings on "
            "the 'arraw' logger, and a document that cannot be read raises ValueError.");

    nb::class_<SettingDescriptor>(m, "SettingDescriptor", "One row of the develop settings table.")
        .def_ro("key", &SettingDescriptor::key)
        .def_ro("name", &SettingDescriptor::name)
        .def_ro("range", &SettingDescriptor::range)
        .def_ro("group", &SettingDescriptor::group)
        .def_ro("applies", &SettingDescriptor::applies)
        .def_ro("affects", &SettingDescriptor::affects)
        .def(nb::self == nb::self)
        .def("__repr__", [](const SettingDescriptor& d) {
            return "SettingDescriptor(name='" + d.name + "', key='" + d.key + "')";
        });
    nb::cast<nb::object>(m.attr("SettingDescriptor")).attr("__hash__") = nb::none();

    m.def("setting_descriptors", &listDescriptors,
          "List the develop settings: key, snake_case name, range, group, applicability, stage.");
}

} // namespace arraw::python
