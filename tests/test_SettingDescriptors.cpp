#include "support/FieldCount.h"

#include <Photo.h>
#include <SettingDescriptors.h>

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

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
        }
    });
    return settings;
}

ImageMetadata someMetadata() {
    return ImageMetadata{ImageSize{8, 8}, workingEncoding};
}

} // namespace

TEST_CASE("The descriptor table has a row per leaf, matching the structs", "[settings]") {
    // A new field needs a descriptor row AND an updated count here.
    STATIC_REQUIRE(test::fieldCount<ToneSettings> == 7);
    STATIC_REQUIRE(test::fieldCount<ColorSettings> == 3);
    STATIC_REQUIRE(test::fieldCount<GeometrySettings> == 5);
    STATIC_REQUIRE(test::fieldCount<CropSettings> == 2);
    STATIC_REQUIRE(test::fieldCount<DevelopSettings> == 3);

    // Leaves: tone + color + geometry (crop is a group of two leaves).
    STATIC_REQUIRE(developSettingDescriptors.size() ==
                   test::fieldCount<ToneSettings> + test::fieldCount<ColorSettings> +
                       test::fieldCount<GeometrySettings> - 1 + test::fieldCount<CropSettings>);
    STATIC_REQUIRE(developSettingDescriptors.size() == 16);
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
        if (!descriptor.range) {
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
    REQUIRE_THROWS_AS(Photo("a.dng", someMetadata(), {.tone = {.exposure = 6.0F}}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(
        Photo("a.dng", someMetadata(), {.color = {.temperature = std::optional<float>{500.0F}}}),
        std::invalid_argument);

    const Photo photo("a.dng", someMetadata());
    REQUIRE_THROWS_AS(photo.with({.geometry = {.straighten = 90.0}}), std::invalid_argument);
    REQUIRE(photo.settings() == DevelopSettings{});
}
