#include "ProcessingPlan.h"
#include "support/FieldCount.h"
#include "support/Fixtures.h"

#include <ImageImport.h>
#include <Photo.h>
#include <SettingDescriptors.h>

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using namespace arraw;

/// The descriptor table (ADR 008): one row per leaf of DevelopSettings, with
/// the ranges validation enforces.

namespace {

/// Sets the field a row describes to something other than its default.
void writeSentinel(const FieldDescriptor& descriptor, DevelopSettings& settings) {
    visitField(descriptor, settings, [](auto& field) {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
            field += 1;
        } else if constexpr (std::is_same_v<T, std::optional<float>>) {
            field = 3000.0F;
        } else if constexpr (std::is_same_v<T, bool>) {
            field = !field;
        } else if constexpr (std::is_same_v<T, WhiteBalanceMode>) {
            field = WhiteBalanceMode::Custom;
        } else if constexpr (std::is_same_v<T, QuarterTurn>) {
            field = QuarterTurn::Clockwise90;
        } else if constexpr (std::is_same_v<T, std::optional<UprightCropRect>>) {
            field = UprightCropRect{.left = 0.1, .top = 0.1, .right = 0.9, .bottom = 0.9};
        } else if constexpr (std::is_same_v<T, ToneCurve>) {
            field.points = {{0.0F, 0.0F}, {0.5F, 0.75F}, {1.0F, 1.0F}};
        } else if constexpr (std::is_same_v<T, GrainModel>) {
            // One model so far: an out-of-table value stands for "another one".
            field = static_cast<GrainModel>(1);
        } else if constexpr (std::is_same_v<T, LuminanceNoiseFilter>) {
            // One filter so far, as for the grain model.
            field = static_cast<LuminanceNoiseFilter>(1);
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
            field += 7U;
        } else {
            static_assert(std::is_same_v<T, CropAspect>);
            field = CropRatio{2.0};
        }
    });
}

/// Whether the field a row describes is the same in two settings.
bool sameField(const FieldDescriptor& descriptor, const DevelopSettings& a,
               const DevelopSettings& b) {
    return visitField(descriptor, a, [&](const auto& x) {
        return visitField(descriptor, b, [&](const auto& y) {
            if constexpr (std::is_same_v<decltype(x), decltype(y)>) {
                return x == y;
            } else {
                return false;
            }
        });
    });
}

/// Sets a ranged row to a number, whatever numeric type it holds.
DevelopSettings withValue(const FieldDescriptor& descriptor, double value) {
    DevelopSettings settings;
    visitField(descriptor, settings, [&](auto& field) {
        using T = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<T, double>) {
            field = value;
        } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, std::optional<float>>) {
            field = static_cast<float>(value);
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
            field = static_cast<std::uint32_t>(value);
        }
    });
    return settings;
}

/// Whether a row's leaf is a whole number, whose type cannot hold a value outside its range.
bool isWholeNumber(const FieldDescriptor& descriptor) {
    return std::holds_alternative<std::uint32_t& (*)(DevelopSettings&)>(descriptor.member);
}

ImageMetadata someMetadata() {
    return ImageMetadata{ImageSize{8, 8}, workingEncoding};
}

} // namespace

TEST_CASE("The descriptor table has a row per leaf, matching the structs", "[settings]") {
    // A new field needs a descriptor row AND an updated count here.
    STATIC_REQUIRE(test::fieldCount<ToneSettings> == 7);
    STATIC_REQUIRE(test::fieldCount<ColorSettings> == 5);
    STATIC_REQUIRE(test::fieldCount<GeometrySettings> == 5);
    STATIC_REQUIRE(test::fieldCount<CropSettings> == 2);
    STATIC_REQUIRE(test::fieldCount<HueBand> == 3);
    STATIC_REQUIRE(test::fieldCount<HslSettings> == 8);
    STATIC_REQUIRE(test::fieldCount<BlackAndWhiteSettings> == 9);
    STATIC_REQUIRE(test::fieldCount<ToneCurveSettings> == 4);
    STATIC_REQUIRE(test::fieldCount<GradeZone> == 2);
    STATIC_REQUIRE(test::fieldCount<ColorGradingSettings> == 5);
    STATIC_REQUIRE(test::fieldCount<VignetteSettings> == 3);
    STATIC_REQUIRE(test::fieldCount<GrainSettings> == 5);
    STATIC_REQUIRE(test::fieldCount<EffectsSettings> == 2);
    STATIC_REQUIRE(test::fieldCount<NoiseReductionSettings> == 5);
    STATIC_REQUIRE(test::fieldCount<PresenceSettings> == 3);
    STATIC_REQUIRE(test::fieldCount<DevelopSettings> == 10);

    // Leaves: tone + color + geometry (crop is a group of two leaves) + hsl
    // (eight bands of three leaves) + black and white + the four tone curves +
    // colour grading (three zones of two leaves, balance and blending) +
    // effects (the vignette's three leaves and the grain's five) + noise
    // reduction + presence.
    STATIC_REQUIRE(
        developSettingDescriptors.size() ==
        test::fieldCount<ToneSettings> + test::fieldCount<ColorSettings> +
            test::fieldCount<GeometrySettings> - 1 + test::fieldCount<CropSettings> +
            test::fieldCount<HslSettings> * test::fieldCount<HueBand> +
            test::fieldCount<BlackAndWhiteSettings> + test::fieldCount<ToneCurveSettings> +
            3 * test::fieldCount<GradeZone> + test::fieldCount<ColorGradingSettings> - 3 +
            test::fieldCount<VignetteSettings> + test::fieldCount<GrainSettings> +
            test::fieldCount<NoiseReductionSettings> + test::fieldCount<PresenceSettings>);
    STATIC_REQUIRE(developSettingDescriptors.size() == 79);
}

TEST_CASE("The vignette and grain rows run in the Effects pass and always apply", "[settings]") {
    int effects = 0;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        if (descriptor.group != SettingGroup::Effects) {
            continue;
        }
        ++effects;
        REQUIRE(descriptor.affects == Stage::Effects);
        REQUIRE(descriptor.applies == Applicability::Always);
    }
    REQUIRE(effects == 8);
    REQUIRE(findDescriptor("vignetteAmount")->range->minimum == -100.0);
    REQUIRE(findDescriptor("vignetteAmount")->range->maximum == 100.0);
    REQUIRE(findDescriptor("vignetteMidpoint")->range->minimum == 0.0);
    REQUIRE(findDescriptor("vignetteFeather")->range->maximum == 100.0);
    const VignetteSettings defaults{};
    REQUIRE(defaults.amount == 0.0F);
    REQUIRE(defaults.midpoint == 50.0F);
    REQUIRE(defaults.feather == 50.0F);
    for (const char* key : {"grainAmount", "grainSize", "grainRoughness"}) {
        INFO(key);
        REQUIRE(findDescriptor(key)->range->minimum == 0.0);
        REQUIRE(findDescriptor(key)->range->maximum == 100.0);
    }
    REQUIRE_FALSE(findDescriptor("grainModel")->range.has_value());
    REQUIRE(findDescriptor("grainSeed")->range->maximum == 4294967295.0);
    const GrainSettings grain{};
    REQUIRE(grain.amount == 0.0F);
    REQUIRE(grain.size == 50.0F);
    REQUIRE(grain.roughness == 50.0F);
    REQUIRE(grain.model == GrainModel::ValueNoise);
    REQUIRE(grain.seed == 0U);
}

TEST_CASE("Only the grain seed belongs to the photograph rather than the look", "[settings]") {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        REQUIRE((descriptor.scope == SettingScope::Photo) == (descriptor.key == "grainSeed"));
    }
}

TEST_CASE("The colour rows are pointwise, always apply and share their groups", "[settings]") {
    int hsl = 0;
    int blackAndWhite = 0;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        hsl += descriptor.group == SettingGroup::Hsl ? 1 : 0;
        blackAndWhite += descriptor.group == SettingGroup::BlackAndWhite ? 1 : 0;
        if (descriptor.group == SettingGroup::Hsl ||
            descriptor.group == SettingGroup::BlackAndWhite) {
            REQUIRE(descriptor.affects == Stage::Pointwise);
            REQUIRE(descriptor.applies == Applicability::Always);
        }
    }
    REQUIRE(hsl == 24);
    REQUIRE(blackAndWhite == 9);
    REQUIRE(findDescriptor("saturation")->group == SettingGroup::Color);
    REQUIRE(findDescriptor("vibrance")->group == SettingGroup::Color);
    REQUIRE(findDescriptor("hueRed")->range->minimum == -100.0);
    REQUIRE(findDescriptor("luminanceMagenta")->range->maximum == 100.0);
    REQUIRE_FALSE(findDescriptor("convertToGrayscale")->range.has_value());
    REQUIRE(findDescriptor("grayBlue") != nullptr);
}

TEST_CASE("The tone curve rows are pointwise, always apply and have no range", "[settings]") {
    int curves = 0;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        if (descriptor.group != SettingGroup::ToneCurve) {
            continue;
        }
        ++curves;
        REQUIRE(descriptor.affects == Stage::Pointwise);
        REQUIRE(descriptor.applies == Applicability::Always);
        REQUIRE_FALSE(descriptor.range.has_value());
    }
    REQUIRE(curves == 4);
    for (const char* key : {"toneCurveLuma", "toneCurveRed", "toneCurveGreen", "toneCurveBlue"}) {
        REQUIRE(findDescriptor(key)->group == SettingGroup::ToneCurve);
    }
}

TEST_CASE("Validation refuses a curve that breaks its invariants", "[settings]") {
    const std::vector<std::vector<CurvePoint>> bad{
        {{0.0F, 0.0F}},
        {{0.0F, 0.0F}, {0.5F, 0.5F}, {0.5F, 0.6F}, {1.0F, 1.0F}},
        {{0.0F, 0.0F}, {0.7F, 0.5F}, {0.3F, 0.6F}, {1.0F, 1.0F}},
        {{0.1F, 0.0F}, {1.0F, 1.0F}},
        {{0.0F, 0.0F}, {0.9F, 1.0F}},
        {{0.0F, 0.0F}, {1.0F, 1.5F}},
        {{0.0F, 0.0F}, {1.0F, std::numeric_limits<float>::quiet_NaN()}},
    };
    for (const auto& points : bad) {
        DevelopSettings settings;
        settings.toneCurve.green.points = points;
        REQUIRE_THROWS_AS(validate(settings), std::invalid_argument);
    }
    DevelopSettings tooMany;
    for (std::size_t i = 0; i <= maximumCurvePoints; ++i) {
        tooMany.toneCurve.red.points.push_back(
            {static_cast<float>(i) / static_cast<float>(maximumCurvePoints), 0.5F});
    }
    REQUIRE_THROWS_AS(validate(tooMany), std::invalid_argument);
    DevelopSettings sixteen;
    sixteen.toneCurve.red.points.clear();
    for (std::size_t i = 0; i < maximumCurvePoints; ++i) {
        sixteen.toneCurve.red.points.push_back(
            {static_cast<float>(i) / static_cast<float>(maximumCurvePoints - 1), 0.5F});
    }
    REQUIRE_NOTHROW(validate(sixteen));
}

TEST_CASE("Keys are unique camelCase names that can be looked up", "[settings]") {
    std::set<std::string_view> seen;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        REQUIRE_FALSE(descriptor.key.empty());
        REQUIRE(std::islower(static_cast<unsigned char>(descriptor.key.front())));
        for (const char c : descriptor.key) {
            REQUIRE(std::isalnum(static_cast<unsigned char>(c)));
        }
        REQUIRE(seen.insert(descriptor.key).second);
        REQUIRE(findDescriptor(descriptor.key) == &descriptor);
    }
    REQUIRE(findDescriptor("noSuchSetting") == nullptr);
}

TEST_CASE("Temperature and tint are the RAW-only rows", "[settings]") {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        const bool raw = descriptor.key == "temperature" || descriptor.key == "tint";
        REQUIRE((descriptor.applies == Applicability::RawOnly) == raw);
    }
}

TEST_CASE("Each default lies within its row's range", "[settings]") {
    const DevelopSettings defaults;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        INFO(descriptor.key);
        if (!descriptor.range) {
            continue;
        }
        visitField(descriptor, defaults, [&](const auto& field) {
            using T = std::remove_cvref_t<decltype(field)>;
            if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
                REQUIRE(field >= descriptor.range->minimum);
                REQUIRE(field <= descriptor.range->maximum);
            } else if constexpr (std::is_same_v<T, std::optional<float>>) {
                // Only temperature and tint: unset by default.
                REQUIRE_FALSE(field.has_value());
            }
        });
    }
}

TEST_CASE("A row writes exactly one field", "[settings]") {
    const DevelopSettings defaults;
    for (const FieldDescriptor& written : developSettingDescriptors) {
        INFO(written.key);
        DevelopSettings settings;
        writeSentinel(written, settings);
        REQUIRE_FALSE(settings == defaults);

        int differing = 0;
        for (const FieldDescriptor& read : developSettingDescriptors) {
            if (!sameField(read, settings, defaults)) {
                ++differing;
                REQUIRE(read.key == written.key);
            }
        }
        REQUIRE(differing == 1);
    }
}

TEST_CASE("A row reaches the field its key names", "[settings]") {
    DevelopSettings settings;
    visitField(*findDescriptor("exposure"), settings, [](auto& field) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, float>) {
            field = 2.0F;
        }
    });
    REQUIRE(settings.tone.exposure == 2.0F);
    REQUIRE(settings.tone.contrast == 0.0F);
}

TEST_CASE("Reading through a const settings object leaves it alone", "[settings]") {
    const DevelopSettings settings{.tone = {.exposure = 1.5F}};
    const FieldDescriptor& exposure = *findDescriptor("exposure");
    const float value = visitField(exposure, settings, [](const auto& field) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, float>) {
            return field;
        } else {
            return 0.0F;
        }
    });
    REQUIRE(value == 1.5F);
    REQUIRE(settings.tone.exposure == 1.5F);
}

TEST_CASE("Validation accepts the defaults and both ends of every range", "[settings]") {
    REQUIRE_NOTHROW(validate(DevelopSettings{}));
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (!descriptor.range) {
            continue;
        }
        INFO(descriptor.key);
        REQUIRE_NOTHROW(validate(withValue(descriptor, descriptor.range->minimum)));
        REQUIRE_NOTHROW(validate(withValue(descriptor, descriptor.range->maximum)));
    }
}

TEST_CASE("Validation refuses a value below, above or outside every range", "[settings]") {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        // A seed's range is its type's: nothing outside it can be stored to refuse.
        if (!descriptor.range || isWholeNumber(descriptor)) {
            continue;
        }
        INFO(descriptor.key);
        const SettingRange range = *descriptor.range;
        REQUIRE_THROWS_AS(validate(withValue(descriptor, range.minimum - 1.0)),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(validate(withValue(descriptor, range.maximum + 1.0)),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(validate(withValue(descriptor, std::nan(""))), std::invalid_argument);
        REQUIRE_THROWS_AS(validate(withValue(descriptor, std::numeric_limits<double>::infinity())),
                          std::invalid_argument);
    }
}

TEST_CASE("A validation failure names the key, the value and the range", "[settings]") {
    try {
        validate({.tone = {.exposure = 9.0F}});
        FAIL("expected an exception");
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        REQUIRE(message == "exposure is 9, outside its range -5 to 5");
    }
}

TEST_CASE("A photograph cannot be built from out-of-range settings", "[settings][photo]") {
    REQUIRE_NOTHROW(Photo("a.dng", someMetadata(), {}));
    REQUIRE_THROWS_AS(Photo("a.dng", someMetadata(), {.settings = {.tone = {.exposure = 6.0F}}}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(Photo("a.dng", someMetadata(),
                            {.settings = {.color = {.temperature = std::optional<float>{500.0F}}}}),
                      std::invalid_argument);

    const Photo photo("a.dng", someMetadata());
    REQUIRE_THROWS_AS(photo.with({.settings = {.geometry = {.straighten = 90.0}}}),
                      std::invalid_argument);
    REQUIRE(photo.state() == DevelopState{});
}

TEST_CASE("Validation refuses a crop that could fit no image", "[settings]") {
    /// Whether a crop fits a particular image is the geometry plan's question;
    /// these fit none, so no photograph may hold them.
    const auto withRectangle = [](UprightCropRect rectangle) {
        return DevelopSettings{.geometry = {.crop = {.rectangle = rectangle}}};
    };
    const auto withRatio = [](double ratio) {
        return DevelopSettings{.geometry = {.crop = {.aspect = CropRatio{ratio}}}};
    };

    REQUIRE_NOTHROW(
        validate(withRectangle({.left = 0.0, .top = 0.0, .right = 1.0, .bottom = 1.0})));
    REQUIRE_NOTHROW(validate(withRatio(0.5)));

    REQUIRE_THROWS_AS(
        validate(withRectangle({.left = 0.6, .top = 0.1, .right = 0.4, .bottom = 0.9})),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        validate(withRectangle({.left = 0.1, .top = 0.5, .right = 0.9, .bottom = 0.5})),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        validate(withRectangle({.left = -0.1, .top = 0.0, .right = 1.0, .bottom = 1.0})),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        validate(withRectangle({.left = 0.0, .top = 0.0, .right = 1.0, .bottom = 1.1})),
        std::invalid_argument);
    REQUIRE_THROWS_AS(validate(withRatio(0.0)), std::invalid_argument);
    REQUIRE_THROWS_AS(validate(withRatio(-1.5)), std::invalid_argument);
}

TEST_CASE("A row's stage is the first boundary its value changes in the plan", "[settings][plan]") {
    // A camera-native source, so that the RAW-only rows reach the plan.
    const ImageBuffer source = loadImage(test::fixture("linear-32x24-skewed.dng"));
    DevelopSettings base;
    // The shaping rows only count once what they shape is on.
    base.noiseReduction.luminance = 40.0F;
    base.noiseReduction.color = 40.0F;
    base.effects.vignette.amount = -30.0F;
    base.effects.grain.amount = 30.0F;
    base.colorGrading.shadows.saturation = 30.0F;
    base.colorGrading.midtones.saturation = 30.0F;
    base.colorGrading.highlights.saturation = 30.0F;
    base.color.temperature = 5000.0F;
    base.color.tint = 10.0F;

    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        // Enumerations with one entry have no other value to change to.
        if (descriptor.key == "grainModel" || descriptor.key == "luminanceNoiseFilter") {
            continue;
        }
        DYNAMIC_SECTION(descriptor.key) {
            DevelopSettings from = base;
            DevelopSettings to = base;
            if (descriptor.key == "temperature" || descriptor.key == "tint") {
                // Only a custom white balance reads them.
                from.color.whiteBalance = WhiteBalanceMode::Custom;
                to.color.whiteBalance = WhiteBalanceMode::Custom;
                if (descriptor.key == "temperature") {
                    to.color.temperature = 4000.0F;
                } else {
                    to.color.tint = 20.0F;
                }
            } else {
                writeSentinel(descriptor, to);
            }
            const ProcessingPlan before = planFor(source, DevelopState{from});
            const ProcessingPlan after = planFor(source, DevelopState{to});
            const auto first = static_cast<std::size_t>(descriptor.affects);
            // Untouched before the boundary, different at it.
            if (first > 0) {
                REQUIRE(prefixMatches(after, before, static_cast<Stage>(first - 1)));
            }
            REQUIRE_FALSE(prefixMatches(after, before, descriptor.affects));
        }
    }
}

TEST_CASE("Every copy section has a Look-scoped row", "[descriptors][sections]") {
    std::set<CopySection> covered;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (descriptor.scope == SettingScope::Look) {
            covered.insert(descriptor.section);
        }
    }
    for (std::size_t i = 0; i < copySectionNames.size(); ++i) {
        CAPTURE(copySectionNames[i]);
        REQUIRE(covered.contains(static_cast<CopySection>(i)));
    }
}

TEST_CASE("Copy section names cover the enumeration in order, uniquely",
          "[descriptors][sections]") {
    REQUIRE(copySectionNames.size() == static_cast<std::size_t>(CopySection::Crop) + 1);
    REQUIRE(copySectionNames.front() == "whiteBalance");
    REQUIRE(copySectionNames[static_cast<std::size_t>(CopySection::Grain)] == "grain");
    REQUIRE(copySectionNames.back() == "crop");
    const std::set<std::string_view> unique(copySectionNames.begin(), copySectionNames.end());
    REQUIRE(unique.size() == copySectionNames.size());
}

TEST_CASE("Descriptor rows sit in the expected copy sections", "[descriptors][sections]") {
    const auto sectionOf = [](std::string_view key) { return findDescriptor(key)->section; };
    REQUIRE(sectionOf("exposure") == CopySection::Exposure);
    REQUIRE(sectionOf("contrast") == CopySection::Tone);
    REQUIRE(sectionOf("temperature") == CopySection::WhiteBalance);
    REQUIRE(sectionOf("saturation") == CopySection::Color);
    REQUIRE(sectionOf("straighten") == CopySection::RotateAndFlip);
    REQUIRE(sectionOf("grainSeed") == CopySection::Grain);
    REQUIRE(findDescriptor("grainSeed")->scope == SettingScope::Photo);
}

TEST_CASE("The copy sections list the keys the plan gives them", "[descriptors][sections]") {
    struct Row {
        CopySection section;
        std::vector<std::string> keys;
    };
    std::vector<Row> table{
        {CopySection::WhiteBalance, {"whiteBalance", "temperature", "tint"}},
        {CopySection::Exposure, {"exposure"}},
        {CopySection::Tone,
         {"contrast", "highlights", "shadows", "whites", "blacks", "filmicHighlights"}},
        {CopySection::Presence, {"texture", "clarity", "dehaze"}},
        {CopySection::Color, {"saturation", "vibrance"}},
        {CopySection::ToneCurve,
         {"toneCurveLuma", "toneCurveRed", "toneCurveGreen", "toneCurveBlue"}},
        {CopySection::Hsl, {}},
        {CopySection::BlackAndWhite, {"convertToGrayscale"}},
        {CopySection::ColorGrading, {"gradeBalance", "gradeBlending"}},
        {CopySection::NoiseReduction,
         {"luminanceNoiseReduction", "luminanceNoiseDetail", "luminanceNoiseFilter",
          "colorNoiseReduction", "colorNoiseSmoothness"}},
        {CopySection::Vignette, {"vignetteAmount", "vignetteMidpoint", "vignetteFeather"}},
        {CopySection::Grain, {"grainAmount", "grainSize", "grainRoughness", "grainModel"}},
        {CopySection::RotateAndFlip, {"rotation", "flipHorizontal", "flipVertical", "straighten"}},
        {CopySection::Crop, {"cropRectangle", "cropAspect"}},
    };
    const auto row = [&table](CopySection section) -> Row& {
        return table[static_cast<std::size_t>(section)];
    };
    for (const char* band :
         {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"}) {
        for (const char* control : {"hue", "saturation", "luminance"}) {
            row(CopySection::Hsl).keys.push_back(std::string(control) + band);
        }
        row(CopySection::BlackAndWhite).keys.push_back(std::string("gray") + band);
    }
    for (const char* zone : {"Shadow", "Midtone", "Highlight"}) {
        for (const char* control : {"Hue", "Saturation"}) {
            row(CopySection::ColorGrading).keys.push_back("grade" + std::string(zone) + control);
        }
    }

    std::set<std::string> listed;
    for (std::size_t i = 0; i < table.size(); ++i) {
        REQUIRE(table[i].section == static_cast<CopySection>(i));
        for (const std::string& key : table[i].keys) {
            CAPTURE(key);
            const FieldDescriptor* descriptor = findDescriptor(key);
            REQUIRE(descriptor != nullptr);
            CHECK(descriptor->section == table[i].section);
            CHECK(descriptor->scope == SettingScope::Look);
            CHECK(listed.insert(key).second);
        }
    }
    std::set<std::string> look;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        if (descriptor.scope == SettingScope::Look) {
            look.insert(std::string(descriptor.key));
        }
    }
    CHECK(listed == look);
}
