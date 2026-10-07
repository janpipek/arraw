// The edit rules of ::arraw::withValue, ::arraw::withValueFrom and ::arraw::withValues, which
// the GUI, the command line and later Python all go through.

#include <Edits.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace arraw;

namespace {

const ImageMetadata photo{ImageSize{32, 24}, workingEncoding};

DevelopState customLight(std::optional<float> kelvin, std::optional<float> tint) {
    DevelopState state;
    state.settings.color = {WhiteBalanceMode::Custom, kelvin, tint};
    return state;
}

DevelopState asShotWithLeftovers() {
    DevelopState state;
    state.settings.color = {WhiteBalanceMode::AsShot, 3000.0F, 9.0F};
    return state;
}

ColorSettings colorOf(const DevelopState& state) {
    return state.settings.color;
}

constexpr std::uint32_t entropyBits = 0xdeadbeefU;

const GrainEntropy drawn = [] { return entropyBits; };

template <class F> void checkThrowsAndKeeps(const DevelopState& state, F&& edit) {
    DevelopState copy = state;
    CHECK_THROWS_AS(edit(copy), std::invalid_argument);
    CHECK(copy == state);
}

} // namespace

// Compile-time rules of which type a setting takes.
static_assert(detail::takes<float, int>());
static_assert(detail::takes<float, double>());
static_assert(!detail::takes<float, bool>());
static_assert(!detail::takes<float, WhiteBalanceMode>());
static_assert(detail::takes<double, float>());
static_assert(detail::takes<std::uint32_t, int>());
static_assert(!detail::takes<std::uint32_t, double>());
static_assert(!detail::takes<std::uint32_t, bool>());
static_assert(detail::takes<std::optional<float>, std::nullopt_t>());
static_assert(detail::takes<std::optional<float>, int>());
static_assert(detail::takes<std::optional<float>, std::optional<double>>());
static_assert(!detail::takes<std::optional<float>, bool>());
static_assert(detail::takes<CropAspect, CropRatio>());
static_assert(detail::takes<CropAspect, FreeCropAspect>());
static_assert(detail::takes<CropAspect, CropAspect>());
static_assert(!detail::takes<CropAspect, double>());
static_assert(detail::takes<bool, bool>());
static_assert(!detail::takes<bool, int>());
static_assert(detail::takes<QuarterTurn, QuarterTurn>());
static_assert(!detail::takes<QuarterTurn, int>());
static_assert(detail::takes<ToneCurve, ToneCurve>());
static_assert(detail::SettingValue<int>);
static_assert(detail::SettingValue<std::nullopt_t>);
static_assert(detail::SettingValue<CropRatio>);
static_assert(!detail::SettingValue<const char*>);
static_assert(!detail::SettingValue<std::vector<int>>);

TEST_CASE("A plain key changes only its own field", "[Edits]") {
    const DevelopState base;
    DevelopState expected = base;
    expected.settings.tone.exposure = 2.0F;
    CHECK(withValue(photo, base, "exposure", 2) == expected);
    CHECK(withValue(photo, base, "exposure", 2.0F) == expected);
    CHECK(withValue(photo, base, "exposure", 2.0) == expected);
}

TEST_CASE("An unknown key throws and leaves the state alone", "[Edits]") {
    checkThrowsAndKeeps(DevelopState{}, [](DevelopState& state) {
        state = withValue(photo, state, "exposureX", 1.0);
    });
    const std::array<std::string_view, 2> keys{"exposure", "nope"};
    checkThrowsAndKeeps(DevelopState{}, [&](DevelopState& state) {
        state = withValues(photo, state, keys, DevelopSettings{});
    });
    checkThrowsAndKeeps(DevelopState{}, [](DevelopState& state) {
        state = withValueFrom(photo, state, "nope", DevelopSettings{});
    });
}

TEST_CASE("A value of a type the setting does not take throws", "[Edits]") {
    const DevelopState base;
    CHECK_THROWS_AS(withValue(photo, base, "exposure", true), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "flipHorizontal", 1), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "grainSeed", 1.0), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "exposure", std::nullopt), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "exposure", WhiteBalanceMode::Custom),
                    std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "rotation", 1), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "cropAspect", 1.5), std::invalid_argument);
}

TEST_CASE("A number outside its range, or not a number, throws", "[Edits]") {
    const DevelopState base;
    constexpr double inf = std::numeric_limits<double>::infinity();
    CHECK_THROWS_AS(withValue(photo, base, "exposure", std::nan("")), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "exposure", inf), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "exposure", -inf), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "exposure", darkestExposure - 0.01),
                    std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "exposure", brightestExposure + 0.01),
                    std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "exposure", 1e300), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "straighten", 46.0), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "temperature", 1000.0), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "grainSeed", -1), std::invalid_argument);
    CHECK_THROWS_AS(withValue(photo, base, "grainSeed", std::int64_t{1} << 32),
                    std::invalid_argument);

    SECTION("a throw leaves the caller's state as it was") {
        checkThrowsAndKeeps(DevelopState{}, [](DevelopState& state) {
            state = withValue(photo, state, "exposure", 99.0);
        });
    }
    SECTION("the exact ends are accepted") {
        CHECK(withValue(photo, base, "exposure", darkestExposure).settings.tone.exposure ==
              darkestExposure);
        CHECK(withValue(photo, base, "exposure", brightestExposure).settings.tone.exposure ==
              brightestExposure);
        CHECK(withValue(photo, base, "straighten", -45.0).settings.geometry.straighten == -45.0);
        CHECK(withValue(photo, base, "grainSeed", 0).settings.effects.grain.seed == 0U);
        CHECK(withValue(photo, base, "grainSeed", 4294967295U).settings.effects.grain.seed ==
              4294967295U);
    }
}

TEST_CASE("Moving Temp leaves Tint where it was", "[Edits][WhiteBalance]") {
    SECTION("from As Shot, tint stays the camera's") {
        CHECK(colorOf(withValue(photo, DevelopState{}, "temperature", 4200.0F)) ==
              ColorSettings{WhiteBalanceMode::Custom, 4200.0F, std::nullopt});
    }
    SECTION("from As Shot with stale values, they are ignored") {
        CHECK(colorOf(withValue(photo, asShotWithLeftovers(), "temperature", 4200.0F)) ==
              ColorSettings{WhiteBalanceMode::Custom, 4200.0F, std::nullopt});
    }
    SECTION("from Custom, a set tint stays") {
        CHECK(colorOf(withValue(photo, customLight(5000.0F, 7.0F), "temperature", 4200.0F)) ==
              ColorSettings{WhiteBalanceMode::Custom, 4200.0F, 7.0F});
    }
}

TEST_CASE("Moving Tint leaves Temp where it was", "[Edits][WhiteBalance]") {
    CHECK(colorOf(withValue(photo, DevelopState{}, "tint", 12.0F)) ==
          ColorSettings{WhiteBalanceMode::Custom, std::nullopt, 12.0F});
    CHECK(colorOf(withValue(photo, customLight(5000.0F, std::nullopt), "tint", 12.0F)) ==
          ColorSettings{WhiteBalanceMode::Custom, 5000.0F, 12.0F});
    CHECK(colorOf(withValue(photo, asShotWithLeftovers(), "tint", 12.0F)) ==
          ColorSettings{WhiteBalanceMode::Custom, std::nullopt, 12.0F});
}

TEST_CASE("Clearing a value clears it, and both cleared is As Shot", "[Edits][WhiteBalance]") {
    const auto cleared = [](DevelopState state, std::string_view key) {
        return colorOf(withValue(photo, std::move(state), key, std::nullopt));
    };
    CHECK(cleared(customLight(5000.0F, 7.0F), "temperature") ==
          ColorSettings{WhiteBalanceMode::Custom, std::nullopt, 7.0F});
    CHECK(cleared(customLight(5000.0F, 7.0F), "tint") ==
          ColorSettings{WhiteBalanceMode::Custom, 5000.0F, std::nullopt});
    CHECK(cleared(customLight(5000.0F, std::nullopt), "temperature") == ColorSettings{});
    CHECK(cleared(customLight(std::nullopt, 7.0F), "tint") == ColorSettings{});
    CHECK(cleared(DevelopState{}, "temperature") == ColorSettings{});
    CHECK(cleared(asShotWithLeftovers(), "tint") == ColorSettings{});
    // An optional of a number, empty or not, is accepted as well.
    CHECK(colorOf(withValue(photo, customLight(5000.0F, 7.0F), "tint", std::optional<float>{})) ==
          ColorSettings{WhiteBalanceMode::Custom, 5000.0F, std::nullopt});
    CHECK(colorOf(withValue(photo, DevelopState{}, "tint", std::optional<double>{3.0})) ==
          ColorSettings{WhiteBalanceMode::Custom, std::nullopt, 3.0F});
}

TEST_CASE("The white balance mode carries its own rules", "[Edits][WhiteBalance]") {
    SECTION("As Shot clears both values") {
        CHECK(colorOf(withValue(photo, customLight(5000.0F, 7.0F), "whiteBalance",
                                WhiteBalanceMode::AsShot)) == ColorSettings{});
    }
    SECTION("Custom from As Shot drops leftovers") {
        CHECK(colorOf(withValue(photo, asShotWithLeftovers(), "whiteBalance",
                                WhiteBalanceMode::Custom)) ==
              ColorSettings{WhiteBalanceMode::Custom, std::nullopt, std::nullopt});
    }
    SECTION("Custom on Custom keeps its values") {
        CHECK(colorOf(withValue(photo, customLight(5000.0F, 7.0F), "whiteBalance",
                                WhiteBalanceMode::Custom)) ==
              ColorSettings{WhiteBalanceMode::Custom, 5000.0F, 7.0F});
    }
}

TEST_CASE("Turning grain on draws a seed", "[Edits][Grain]") {
    const auto grainOf = [](const DevelopState& state) { return state.settings.effects.grain; };
    SECTION("from zero with no seed, the entropy's bits") {
        CHECK(grainOf(withValue(photo, DevelopState{}, "grainAmount", 40.0, drawn)).seed ==
              entropyBits);
    }
    SECTION("entropy that returns zero gives one") {
        CHECK(grainOf(withValue(photo, DevelopState{}, "grainAmount", 40.0, [] {
                  return 0U;
              })).seed == 1U);
    }
    SECTION("grain that is already on keeps its seed") {
        DevelopState on;
        on.settings.effects.grain.amount = 20.0F;
        on.settings.effects.grain.seed = 0U;
        CHECK(grainOf(withValue(photo, on, "grainAmount", 60.0, drawn)).seed == 0U);
        on.settings.effects.grain.seed = 9U;
        CHECK(grainOf(withValue(photo, on, "grainAmount", 60.0, drawn)).seed == 9U);
    }
    SECTION("a seed already chosen is kept") {
        DevelopState seeded;
        seeded.settings.effects.grain.seed = 5U;
        CHECK(grainOf(withValue(photo, seeded, "grainAmount", 40.0, drawn)).seed == 5U);
    }
    SECTION("turning it off keeps the seed") {
        DevelopState on;
        on.settings.effects.grain.amount = 20.0F;
        on.settings.effects.grain.seed = 5U;
        const GrainSettings off = grainOf(withValue(photo, on, "grainAmount", 0.0, drawn));
        CHECK(off.amount == 0.0F);
        CHECK(off.seed == 5U);
    }
    SECTION("another grain key never draws") {
        CHECK(grainOf(withValue(photo, DevelopState{}, "grainSize", 40.0, drawn)).seed == 0U);
    }
}

TEST_CASE("A copy takes the value the source holds", "[Edits]") {
    DevelopSettings source;
    source.tone.exposure = 1.5F;
    source.tone.contrast = 30.0F;
    const DevelopState result = withValueFrom(photo, DevelopState{}, "exposure", source);
    CHECK(result.settings.tone.exposure == 1.5F);
    CHECK(result.settings.tone.contrast == 0.0F);
}

TEST_CASE("A copy reads the light as the source's mode means it", "[Edits]") {
    SECTION("an As Shot source with leftovers copies as absent") {
        const DevelopSettings source = asShotWithLeftovers().settings;
        CHECK(colorOf(withValueFrom(photo, customLight(5000.0F, 7.0F), "temperature", source)) ==
              ColorSettings{WhiteBalanceMode::Custom, std::nullopt, 7.0F});
        CHECK(colorOf(withValueFrom(photo, DevelopState{}, "temperature", source)) ==
              ColorSettings{});
    }
    SECTION("a Custom source copies its value") {
        const DevelopSettings source = customLight(4400.0F, std::nullopt).settings;
        CHECK(colorOf(withValueFrom(photo, DevelopState{}, "temperature", source)).temperature ==
              4400.0F);
    }
}

TEST_CASE("A copy validates the value of the source", "[Edits]") {
    DevelopSettings source;
    source.tone.exposure = 99.0F;
    source.color = {WhiteBalanceMode::Custom, 1000.0F, std::nullopt};
    source.geometry.crop.rectangle = UprightCropRect{.left = 0.8, .right = 0.2};
    checkThrowsAndKeeps(DevelopState{}, [&](DevelopState& state) {
        state = withValueFrom(photo, state, "exposure", source);
    });
    checkThrowsAndKeeps(DevelopState{}, [&](DevelopState& state) {
        state = withValueFrom(photo, state, "temperature", source);
    });
    checkThrowsAndKeeps(DevelopState{}, [&](DevelopState& state) {
        state = withValueFrom(photo, state, "cropRectangle", source);
    });
    // Only the key's own field is checked.
    CHECK_NOTHROW(withValueFrom(photo, DevelopState{}, "contrast", source));
}

TEST_CASE("Several settings apply in table order, once each", "[Edits]") {
    DevelopSettings source;
    source.effects.grain.amount = 50.0F;
    source.effects.grain.seed = 77U;
    source.tone.exposure = 1.0F;

    SECTION("an explicit seed wins over a drawn one, whatever the order of the keys") {
        const std::array<std::string_view, 2> a{"grainSeed", "grainAmount"};
        const std::array<std::string_view, 2> b{"grainAmount", "grainSeed"};
        const DevelopState base;
        const DevelopState first = withValues(photo, base, a, source, drawn);
        CHECK(first.settings.effects.grain.seed == 77U);
        CHECK(first == withValues(photo, base, b, source, drawn));
    }
    SECTION("a seed of zero named wins too") {
        source.effects.grain.seed = 0U;
        const std::array<std::string_view, 2> keys{"grainAmount", "grainSeed"};
        CHECK(withValues(photo, DevelopState{}, keys, source, drawn).settings.effects.grain.seed ==
              0U);
    }
    SECTION("the amount alone draws a seed") {
        const std::array<std::string_view, 1> keys{"grainAmount"};
        CHECK(withValues(photo, DevelopState{}, keys, source, drawn).settings.effects.grain.seed ==
              entropyBits);
    }
    SECTION("duplicates apply once") {
        const std::array<std::string_view, 3> keys{"exposure", "exposure", "exposure"};
        DevelopState expected;
        expected.settings.tone.exposure = 1.0F;
        CHECK(withValues(photo, DevelopState{}, keys, source) == expected);
    }
    SECTION("no keys change nothing") {
        const std::vector<std::string_view> none;
        const DevelopState base = customLight(5000.0F, 7.0F);
        CHECK(withValues(photo, base, none, source) == base);
    }
}

TEST_CASE("Setting the white balance keys keeps saturation and vibrance", "[Edits][WhiteBalance]") {
    DevelopState state = asShotWithLeftovers();
    state.settings.color.saturation = 25.0F;
    state.settings.color.vibrance = -10.0F;
    DevelopSettings source;
    source.color = {WhiteBalanceMode::Custom, 5500.0F, 10.0F};
    constexpr std::array<std::string_view, 3> keys{"whiteBalance", "temperature", "tint"};

    const ColorSettings custom = colorOf(withValues(photo, state, keys, source));
    CHECK(custom.whiteBalance == WhiteBalanceMode::Custom);
    CHECK(custom.temperature == 5500.0F);
    CHECK(custom.tint == 10.0F);
    CHECK(custom.saturation == 25.0F);
    CHECK(custom.vibrance == -10.0F);

    const ColorSettings asShot = colorOf(withValues(photo, state, keys, DevelopSettings{}));
    CHECK(asShot.whiteBalance == WhiteBalanceMode::AsShot);
    CHECK(asShot.saturation == 25.0F);
    CHECK(asShot.vibrance == -10.0F);
}

TEST_CASE("The white balance section copies as one", "[Edits][WhiteBalance]") {
    const std::array<std::string_view, 3> keys{"tint", "temperature", "whiteBalance"};
    const DevelopState target = customLight(3000.0F, -4.0F);

    SECTION("Custom with a temperature only") {
        const DevelopSettings source = customLight(5000.0F, std::nullopt).settings;
        CHECK(colorOf(withValues(photo, target, keys, source)) ==
              ColorSettings{WhiteBalanceMode::Custom, 5000.0F, std::nullopt});
    }
    SECTION("As Shot with leftovers is As Shot without values") {
        CHECK(colorOf(withValues(photo, target, keys, asShotWithLeftovers().settings)) ==
              ColorSettings{});
        CHECK(colorOf(withValues(photo, asShotWithLeftovers(), keys,
                                 asShotWithLeftovers().settings)) == ColorSettings{});
    }
    SECTION("Custom with no values renders as As Shot") {
        CHECK(colorOf(withValues(photo, target, keys,
                                 customLight(std::nullopt, std::nullopt).settings)) ==
              ColorSettings{});
    }
    SECTION("Custom with both values") {
        CHECK(
            colorOf(withValues(photo, DevelopState{}, keys, customLight(5000.0F, 7.0F).settings)) ==
            ColorSettings{WhiteBalanceMode::Custom, 5000.0F, 7.0F});
    }
}

TEST_CASE("Geometry is a plain assignment for now", "[Edits][Geometry]") {
    SECTION("a rotation leaves an explicit crop untouched") {
        DevelopState base;
        const UprightCropRect crop{.left = 0.1, .top = 0.2, .right = 0.6, .bottom = 0.9};
        base.settings.geometry.crop.rectangle = crop;
        const DevelopState turned = withValue(photo, base, "rotation", QuarterTurn::Clockwise90);
        CHECK(turned.settings.geometry.rotation == QuarterTurn::Clockwise90);
        CHECK(turned.settings.geometry.crop.rectangle == crop);
    }
    SECTION("a well-formed crop is stored and nullopt clears it") {
        const UprightCropRect crop{.left = 0.1, .top = 0.1, .right = 0.9, .bottom = 0.9};
        const DevelopState cropped = withValue(photo, DevelopState{}, "cropRectangle", crop);
        CHECK(cropped.settings.geometry.crop.rectangle == crop);
        CHECK_FALSE(withValue(photo, cropped, "cropRectangle", std::nullopt)
                        .settings.geometry.crop.rectangle);
    }
    SECTION("an aspect is any alternative") {
        CHECK(withValue(photo, DevelopState{}, "cropAspect", CropRatio{1.5})
                  .settings.geometry.crop.aspect == CropAspect{CropRatio{1.5}});
        CHECK(withValue(photo, DevelopState{}, "cropAspect", OriginalCropAspect{})
                  .settings.geometry.crop.aspect == CropAspect{OriginalCropAspect{}});
    }
    SECTION("malformed values throw") {
        CHECK_THROWS_AS(withValue(photo, DevelopState{}, "cropRectangle",
                                  UprightCropRect{.left = 0.9, .right = 0.1}),
                        std::invalid_argument);
        CHECK_THROWS_AS(withValue(photo, DevelopState{}, "cropAspect", CropRatio{0.0}),
                        std::invalid_argument);
        ToneCurve bad;
        bad.points = {{0.5F, 0.5F}};
        CHECK_THROWS_AS(withValue(photo, DevelopState{}, "toneCurveLuma", bad),
                        std::invalid_argument);
    }
    SECTION("a well-formed curve is stored") {
        ToneCurve curve;
        curve.points = {{0.0F, 0.0F}, {0.5F, 0.75F}, {1.0F, 1.0F}};
        CHECK(withValue(photo, DevelopState{}, "toneCurveRed", curve).settings.toneCurve.red ==
              curve);
    }
}

TEST_CASE("Every setting can be copied from a source that differs from the default", "[Edits]") {
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        CAPTURE(descriptor.key);
        DevelopSettings source;
        visitField(descriptor, source, [](auto& field) {
            using T = std::remove_cvref_t<decltype(field)>;
            if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
                field += 1;
            } else if constexpr (std::is_same_v<T, std::optional<float>>) {
                field = 30.0F;
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
                field = static_cast<LuminanceNoiseFilter>(1);
            } else if constexpr (std::is_same_v<T, std::uint32_t>) {
                field += 7U;
            } else {
                static_assert(std::is_same_v<T, CropAspect>);
                field = CropRatio{2.0};
            }
        });
        if (descriptor.key == "temperature" || descriptor.key == "tint") {
            source.color.whiteBalance = WhiteBalanceMode::Custom;
            if (descriptor.key == "temperature") {
                source.color.temperature = 5000.0F;
            }
        }
        const DevelopState result =
            withValueFrom(photo, DevelopState{}, descriptor.key, source, drawn);
        visitField(descriptor, result.settings, [&](const auto& got) {
            visitField(descriptor, source, [&](const auto& wanted) {
                if constexpr (std::is_same_v<decltype(got), decltype(wanted)>) {
                    CHECK(got == wanted);
                }
            });
        });
        // The one-element enumerations differ from a default by an out-of-table value only.
        CHECK_FALSE(result == DevelopState{});
    }
}

namespace {

const ImageMetadata rawPhoto{ImageSize{32, 24}, CameraNative{}};

DevelopState withSettings(const DevelopSettings& settings) {
    DevelopState state;
    state.settings = settings;
    return state;
}

const CopySection allCopyable[] = {
    CopySection::WhiteBalance, CopySection::Exposure,
    CopySection::Tone,         CopySection::Presence,
    CopySection::Color,        CopySection::ToneCurve,
    CopySection::Hsl,          CopySection::BlackAndWhite,
    CopySection::ColorGrading, CopySection::NoiseReduction,
    CopySection::Vignette,     CopySection::Grain,
};

} // namespace

TEST_CASE("A look is taken with whether its photograph is a RAW", "[Edits][Look]") {
    CHECK(lookOf(rawPhoto, {}).fromRaw);
    CHECK_FALSE(lookOf(photo, {}).fromRaw);
    DevelopSettings settings;
    settings.tone.exposure = 1.0F;
    CHECK(lookOf(photo, settings).settings == settings);
}

TEST_CASE("The copyable sections are consistent with the table", "[Edits][Look]") {
    for (const CopySection section : copyableSections) {
        CHECK(section != CopySection::RotateAndFlip);
        CHECK(section != CopySection::Crop);
        const bool hasRow =
            std::ranges::any_of(developSettingDescriptors, [&](const FieldDescriptor& d) {
                return d.section == section && d.scope == SettingScope::Look;
            });
        CHECK(hasRow);
    }
    for (const CopySection section : defaultCopySections) {
        CHECK(std::ranges::find(copyableSections, section) != copyableSections.end());
    }
}

TEST_CASE("A look replaces the sections chosen and leaves the others", "[Edits][Look]") {
    DevelopSettings source;
    source.tone.exposure = 1.5F;
    source.tone.contrast = 20.0F;
    source.color.saturation = 10.0F;
    DevelopSettings target;
    target.tone.exposure = -1.0F;
    target.tone.contrast = -30.0F;
    target.color.saturation = -40.0F;
    const Look look = lookOf(photo, source);

    SECTION("exposure alone leaves the contrast") {
        const CopySection sections[] = {CopySection::Exposure};
        const AppliedLook applied = withLook(photo, withSettings(target), look, sections);
        CHECK(applied.state.settings.tone.exposure == 1.5F);
        CHECK(applied.state.settings.tone.contrast == -30.0F);
        CHECK(applied.state.settings.color.saturation == -40.0F);
        CHECK(applied.skipped.empty());
    }
    SECTION("tone alone leaves the exposure") {
        const CopySection sections[] = {CopySection::Tone};
        const AppliedLook applied = withLook(photo, withSettings(target), look, sections);
        CHECK(applied.state.settings.tone.exposure == -1.0F);
        CHECK(applied.state.settings.tone.contrast == 20.0F);
    }
    SECTION("a section is replaced in full, defaults included") {
        DevelopSettings curved = target;
        curved.toneCurve.luma.points = {{0.0F, 0.0F}, {0.5F, 0.3F}, {1.0F, 1.0F}};
        const CopySection sections[] = {CopySection::ToneCurve};
        const AppliedLook applied = withLook(photo, withSettings(curved), look, sections);
        CHECK(applied.state.settings.toneCurve == ToneCurveSettings{});
    }
    SECTION("order and duplicates do not matter") {
        const CopySection one[] = {CopySection::Exposure, CopySection::Tone, CopySection::Exposure};
        const CopySection two[] = {CopySection::Tone, CopySection::Exposure};
        CHECK(withLook(photo, withSettings(target), look, one) ==
              withLook(photo, withSettings(target), look, two));
    }
    SECTION("no sections change nothing") {
        const AppliedLook applied = withLook(photo, withSettings(target), look, {});
        CHECK(applied == AppliedLook{withSettings(target), {}});
    }
}

TEST_CASE("A look never carries the grain seed of its photograph", "[Edits][Look][Grain]") {
    const auto grainOf = [](const AppliedLook& a) { return a.state.settings.effects.grain; };
    DevelopSettings source;
    source.effects.grain.amount = 30.0F;
    source.effects.grain.seed = 77U;
    const Look look = lookOf(photo, source);
    const CopySection sections[] = {CopySection::Grain};

    SECTION("a target with no grain and no seed draws one") {
        const AppliedLook applied = withLook(photo, DevelopState{}, look, sections, drawn);
        CHECK(grainOf(applied).amount == 30.0F);
        CHECK(grainOf(applied).seed == entropyBits);
    }
    SECTION("a target with grain keeps its seed") {
        DevelopSettings target;
        target.effects.grain.amount = 10.0F;
        target.effects.grain.seed = 5U;
        CHECK(grainOf(withLook(photo, withSettings(target), look, sections, drawn)).seed == 5U);
    }
    SECTION("a seed from the look is not copied with every section") {
        const AppliedLook applied = withLook(photo, DevelopState{}, look, allCopyable, drawn);
        CHECK(grainOf(applied).seed == entropyBits);
    }
    SECTION("a look without grain turns it off and keeps the seed") {
        DevelopSettings target;
        target.effects.grain.amount = 10.0F;
        target.effects.grain.seed = 5U;
        const Look none = lookOf(photo, {});
        const AppliedLook applied = withLook(photo, withSettings(target), none, sections, drawn);
        CHECK(grainOf(applied).amount == 0.0F);
        CHECK(grainOf(applied).seed == 5U);
    }
}

TEST_CASE("The white balance crosses only between photographs it applies to", "[Edits][Look]") {
    DevelopSettings custom;
    custom.color = {WhiteBalanceMode::Custom, 5000.0F, 7.0F};
    custom.color.saturation = 25.0F;
    custom.color.vibrance = 15.0F;
    DevelopSettings target;
    target.color = {WhiteBalanceMode::Custom, 3000.0F, -5.0F};
    const CopySection wb[] = {CopySection::WhiteBalance};

    SECTION("RAW to JPEG keeps the target's colour and reports it") {
        const AppliedLook applied =
            withLook(photo, withSettings(target), lookOf(rawPhoto, custom), wb);
        CHECK(applied.state.settings.color == target.color);
        CHECK(applied.skipped == std::vector{CopySection::WhiteBalance});
    }
    SECTION("JPEG to RAW keeps the target's colour and reports it") {
        const AppliedLook applied =
            withLook(rawPhoto, withSettings(target), lookOf(photo, custom), wb);
        CHECK(applied.state.settings.color == target.color);
        CHECK(applied.skipped == std::vector{CopySection::WhiteBalance});
    }
    SECTION("RAW to RAW carries Custom") {
        const AppliedLook applied =
            withLook(rawPhoto, withSettings(target), lookOf(rawPhoto, custom), wb);
        CHECK(applied.state.settings.color.whiteBalance == WhiteBalanceMode::Custom);
        CHECK(applied.state.settings.color.temperature == 5000.0F);
        CHECK(applied.state.settings.color.tint == 7.0F);
        CHECK(applied.skipped.empty());
    }
    SECTION("an As Shot source with leftovers arrives as As Shot") {
        DevelopSettings leftovers;
        leftovers.color = {WhiteBalanceMode::AsShot, 3000.0F, 9.0F};
        const AppliedLook applied =
            withLook(rawPhoto, withSettings(target), lookOf(rawPhoto, leftovers), wb);
        CHECK(applied.state.settings.color == ColorSettings{});
    }
    SECTION("JPEG to JPEG copies nothing and reports nothing") {
        const AppliedLook applied =
            withLook(photo, withSettings(target), lookOf(photo, custom), wb);
        CHECK(applied.state == withSettings(target));
        CHECK(applied.skipped.empty());
    }
    SECTION("the colour section still crosses") {
        const CopySection sections[] = {CopySection::WhiteBalance, CopySection::Color};
        const AppliedLook applied =
            withLook(photo, withSettings(target), lookOf(rawPhoto, custom), sections);
        CHECK(applied.state.settings.color.saturation == 25.0F);
        CHECK(applied.state.settings.color.vibrance == 15.0F);
        CHECK(applied.state.settings.color.temperature == 3000.0F);
        CHECK(applied.skipped == std::vector{CopySection::WhiteBalance});
    }
}

TEST_CASE("A look with every copyable section leaves the target's geometry alone",
          "[Edits][Look]") {
    DevelopSettings source;
    source.tone.exposure = 1.0F;
    source.geometry.crop.rectangle = UprightCropRect{0.1, 0.1, 0.9, 0.9};
    source.geometry.straighten = 3.0;
    source.geometry.rotation = QuarterTurn::Clockwise90;
    DevelopSettings target;
    target.geometry.crop.rectangle = UprightCropRect{0.2, 0.2, 0.7, 0.8};
    const AppliedLook applied =
        withLook(photo, withSettings(target), lookOf(photo, source), allCopyable);
    CHECK(applied.state.settings.geometry == target.geometry);
    CHECK(applied.state.settings.tone.exposure == 1.0F);
}

TEST_CASE("A look refuses the geometry sections and invalid values", "[Edits][Look]") {
    DevelopSettings source;
    source.tone.exposure = 1.0F;
    const Look look = lookOf(photo, source);
    DevelopState state;
    state.settings.tone.exposure = -1.0F;

    SECTION("a geometry section throws, naming it, and nothing is applied") {
        const CopySection rotate[] = {CopySection::Exposure, CopySection::RotateAndFlip};
        CHECK_THROWS_WITH(withLook(photo, state, look, rotate),
                          Catch::Matchers::ContainsSubstring("rotateAndFlip"));
        const CopySection crop[] = {CopySection::Crop, CopySection::Exposure};
        CHECK_THROWS_WITH(withLook(photo, state, look, crop),
                          Catch::Matchers::ContainsSubstring("crop"));
    }
    SECTION("a value out of range throws") {
        Look bad = look;
        bad.settings.tone.exposure = 1000.0F;
        const CopySection sections[] = {CopySection::Exposure};
        CHECK_THROWS_AS(withLook(photo, state, bad, sections), std::invalid_argument);
    }
    SECTION("an unknown enumerator throws") {
        const CopySection sections[] = {static_cast<CopySection>(99)};
        CHECK_THROWS_AS(withLook(photo, state, look, sections), std::invalid_argument);
    }
}

TEST_CASE("Only White Balance has nothing to carry from a photograph that is not a RAW",
          "[Edits][Look]") {
    for (const CopySection section : copyableSections) {
        CAPTURE(copySectionNames[static_cast<std::size_t>(section)]);
        CHECK(sectionApplies(section, true));
        CHECK(sectionApplies(section, false) == (section != CopySection::WhiteBalance));
    }
}
