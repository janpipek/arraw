#include "ProcessingPlan.h"

#include <GeometrySettings.h>
#include <WhiteBalance.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace arraw;

namespace {

/// @brief CIE XYZ (D65) to linear Rec.2020.
///
/// Published reference data, deliberately not shared with the engine: a test
/// that used the same constant as the code could not catch a wrong one.
constexpr Matrix3 xyzToRec2020{{1.7166512F, -0.3556708F, -0.2533663F, //
                                -0.6666844F, 1.6164812F, 0.0157685F,  //
                                0.0176399F, -0.0427706F, 0.9421031F}};

/// @brief Builds a camera that records a given light as neutral.
///
/// Its channels are the working space's own primaries, so the only thing under
/// test is the temperature maths rather than a change of primaries as well.
/// @param x Chromaticity of the light.
/// @param y Chromaticity of the light.
/// @return A sensor whose recorded white balance neutralises that light.
CameraNative cameraSeeing(float x, float y) {
    const std::array<float, 3> xyz{x / y, 1.0F, (1.0F - x - y) / y};
    const auto neutral = xyzToRec2020 * xyz;

    CameraNative camera;
    camera.asShotMultipliers =
        withGreenAtOne({1.0F / neutral[0], 1.0F / neutral[1], 1.0F / neutral[2]});
    camera.appliedMultipliers = camera.asShotMultipliers;
    return camera;
}

} // namespace

TEST_CASE("Published illuminants read back at their published temperatures", "[whitebalance]") {
    /// The check that pins the maths to something outside arraw. Illuminant A
    /// is a glowing filament, so it sits *on* the curve and its tint is zero;
    /// daylight sits a little beside it, which is what the small positive tint
    /// on D65 and D50 means.
    SECTION("Illuminant A, a tungsten filament at 2856 K") {
        const auto reading = asShotTemperature(cameraSeeing(0.44757F, 0.40745F));
        REQUIRE(std::abs(reading.kelvin - 2856.0F) < 5.0F);
        REQUIRE(std::abs(reading.tint) < 0.5F);
    }
    SECTION("D50, the printing standard at 5003 K") {
        const auto reading = asShotTemperature(cameraSeeing(0.34567F, 0.35850F));
        REQUIRE(std::abs(reading.kelvin - 5003.0F) < 5.0F);
        REQUIRE(reading.tint > 5.0F);
    }
    SECTION("D65, the daylight standard at 6504 K") {
        const auto reading = asShotTemperature(cameraSeeing(0.31272F, 0.32903F));
        REQUIRE(std::abs(reading.kelvin - 6504.0F) < 5.0F);
        REQUIRE(reading.tint > 5.0F);
    }
}

TEST_CASE("A temperature survives the trip into gains and back", "[whitebalance]") {
    const float kelvin = GENERATE(2000.0F, 3200.0F, 4500.0F, 5500.0F, 6500.0F, 9000.0F, 12000.0F);
    const float tint = GENERATE(-40.0F, 0.0F, 25.0F);

    CameraNative camera = cameraSeeing(0.31272F, 0.32903F);
    camera.asShotMultipliers = whiteBalanceGains(camera, {kelvin, tint});

    const auto reading = asShotTemperature(camera);

    CAPTURE(kelvin, tint, reading.kelvin, reading.tint);
    REQUIRE(std::abs(reading.kelvin - kelvin) < 1.0F);
    REQUIRE(std::abs(reading.tint - tint) < 0.1F);
}

TEST_CASE("A lower temperature cools the photograph", "[whitebalance]") {
    const CameraNative camera = cameraSeeing(0.31272F, 0.32903F);

    const auto warmLight = whiteBalanceGains(camera, {2800.0F, 0.0F});
    const auto coolLight = whiteBalanceGains(camera, {9000.0F, 0.0F});

    /// Telling arraw the light was warm makes it take red back out, which is
    /// the direction the slider moves in every other editor: left is bluer.
    REQUIRE(warmLight[0] < coolLight[0]);
    REQUIRE(warmLight[2] > coolLight[2]);
}

TEST_CASE("Tint moves across the temperature, not along it", "[whitebalance]") {
    const CameraNative camera = cameraSeeing(0.31272F, 0.32903F);

    const auto neutral = whiteBalanceGains(camera, {5500.0F, 0.0F});
    const auto magenta = whiteBalanceGains(camera, {5500.0F, 40.0F});
    const auto green = whiteBalanceGains(camera, {5500.0F, -40.0F});

    /// Green is pinned at 1, so "more magenta" shows up as red and blue rising
    /// together -- which is exactly what it is: everything except green.
    REQUIRE(magenta[0] > neutral[0]);
    REQUIRE(magenta[2] > neutral[2]);
    REQUIRE(green[0] < neutral[0]);
    REQUIRE(green[2] < neutral[2]);
}

TEST_CASE("Two sensors with the same matrix still balance differently", "[whitebalance]") {
    /// The reason CameraNative carries a daylight calibration at all. LibRaw
    /// normalises the camera matrix's rows and keeps the divisors separately,
    /// so these two cameras share a matrix and are not the same sensor. If the
    /// calibration were dropped, this test would see one camera.
    CameraNative even = cameraSeeing(0.31272F, 0.32903F);
    CameraNative uneven = even;
    uneven.daylightScale = {0.5F, 1.0F, 1.25F};

    const auto evenGains = whiteBalanceGains(even, {5000.0F, 0.0F});
    const auto unevenGains = whiteBalanceGains(uneven, {5000.0F, 0.0F});

    REQUIRE(std::abs(evenGains[0] - unevenGains[0]) > 0.1F);
    REQUIRE(std::abs(evenGains[2] - unevenGains[2]) > 0.1F);
}

TEST_CASE("Gains are rescaled about green", "[whitebalance]") {
    const auto gains = withGreenAtOne({4.0F, 2.0F, 1.0F});

    REQUIRE(gains[0] == 2.0F);
    REQUIRE(gains[1] == 1.0F);
    REQUIRE(gains[2] == 0.5F);
    REQUIRE_THROWS_AS(withGreenAtOne({1.0F, 0.0F, 1.0F}), std::invalid_argument);
}

TEST_CASE("A light outside the modelled range is brought inside it", "[whitebalance]") {
    /// The processing contract clamps whatever reaches it, so that no pixel
    /// maths depends on a caller having checked first (ADR 008). A sidecar
    /// written by another editor with a wider range is the reason this matters.
    const CameraNative camera = cameraSeeing(0.31272F, 0.32903F);

    REQUIRE(whiteBalanceGains(camera, {1500.0F, 0.0F}) ==
            whiteBalanceGains(camera, {warmestKelvin, 0.0F}));
    REQUIRE(whiteBalanceGains(camera, {30000.0F, 0.0F}) ==
            whiteBalanceGains(camera, {coolestKelvin, 0.0F}));
    REQUIRE(whiteBalanceGains(camera, {0.0F, 0.0F}) ==
            whiteBalanceGains(camera, {warmestKelvin, 0.0F}));
    REQUIRE(whiteBalanceGains(camera, {5500.0F, 400.0F}) ==
            whiteBalanceGains(camera, {5500.0F, tintLimit}));
}

TEST_CASE("A light that is not a number is refused", "[whitebalance]") {
    /// Clamping cannot rescue these: there is no nearest valid temperature to
    /// a NaN, and silently choosing one would put arbitrary colour in a file.
    const CameraNative camera = cameraSeeing(0.31272F, 0.32903F);
    const float notANumber = std::numeric_limits<float>::quiet_NaN();
    const float infinite = std::numeric_limits<float>::infinity();

    REQUIRE_THROWS_AS(whiteBalanceGains(camera, {notANumber, 0.0F}), std::invalid_argument);
    REQUIRE_THROWS_AS(whiteBalanceGains(camera, {5500.0F, notANumber}), std::invalid_argument);
    REQUIRE_THROWS_AS(whiteBalanceGains(camera, {infinite, 0.0F}), std::invalid_argument);
    REQUIRE_THROWS_AS(whiteBalanceGains(camera, {-infinite, 0.0F}), std::invalid_argument);
    REQUIRE_THROWS_AS(temperatureForGains(camera, {notANumber, 1.0F, 1.0F}), std::invalid_argument);
    REQUIRE_THROWS_AS(temperatureForGains(camera, {1.0F, 1.0F, 0.0F}), std::invalid_argument);
}

TEST_CASE("A light a narrow sensor cannot see is held to a floor", "[whitebalance]") {
    /// A set of primaries as tight as sRGB's cannot represent a deep tungsten
    /// glow with three positive numbers. Refusing would abandon a file for a
    /// request a photographer would call ordinary, so the response is held to a
    /// floor instead -- and the gain between channels is bounded with it.
    CameraNative narrow = cameraSeeing(0.31272F, 0.32903F);
    narrow.toWorking = Matrix3{{0.6274039F, 0.3292830F, 0.0433131F, //
                                0.0690973F, 0.9195404F, 0.0113623F, //
                                0.0163914F, 0.0880133F, 0.8955953F}};

    const auto gains = whiteBalanceGains(narrow, {warmestKelvin, 0.0F});

    for (const float gain : gains) {
        REQUIRE(std::isfinite(gain));
        REQUIRE(gain > 0.0F);
    }
    const auto widest = std::max({gains[0], gains[1], gains[2]});
    const auto narrowest = std::min({gains[0], gains[1], gains[2]});
    REQUIRE(widest / narrowest <= 1000.0F);
}

namespace {

/// @brief What the decode hands over for a grey card lit by a given light.
///
/// The card reflects the light, so the sensor records the reciprocal of the
/// gains that would neutralise it; the decode then multiplies by the gains it
/// applied. Built from whiteBalanceGains, so the test and the engine agree on
/// what a temperature means without sharing the code that reads one back.
std::array<float, 3> cardUnder(const CameraNative& camera, ColourTemperature light) {
    const Gains gains = whiteBalanceGains(camera, light);
    const Gains applied = withGreenAtOne(camera.appliedMultipliers);
    return {applied[0] / gains[0], applied[1] / gains[1], applied[2] / gains[2]};
}

/// @brief Builds a photograph whose pixels are chosen by a function of position.
ImageBuffer
photographWith(const CameraNative& camera, ImageSize size,
               const std::function<std::array<float, 3>(std::uint32_t, std::uint32_t)>& pixelAt) {
    ImageBuffer buffer(size, PixelFormat::RgbaF32, camera);
    auto samples = buffer.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const auto pixel = pixelAt(x, y);
            const std::size_t at = (static_cast<std::size_t>(y) * size.width + x) * 4;
            samples[at] = pixel[0];
            samples[at + 1] = pixel[1];
            samples[at + 2] = pixel[2];
            samples[at + 3] = 1.0F;
        }
    }
    return buffer;
}

/// A camera whose decode applied gains unlike its recorded ones, so that a
/// pick which compared against the wrong set would be off by a lot.
CameraNative camera() {
    CameraNative camera = cameraSeeing(0.31272F, 0.32903F);
    camera.appliedMultipliers = {2.1F, 1.0F, 1.4F};
    return camera;
}

} // namespace

TEST_CASE("Picking a grey card recovers the light it was lit by", "[whitebalance][pick]") {
    const CameraNative cam = camera();
    const ColourTemperature light =
        GENERATE(ColourTemperature{3200.0F, 0.0F}, ColourTemperature{5500.0F, 12.0F},
                 ColourTemperature{7500.0F, -20.0F});
    const auto card = cardUnder(cam, light);
    const ImageBuffer photo = photographWith(cam, {16, 12}, [&](auto, auto) { return card; });

    const auto picked = neutralTemperatureAt(photo, DevelopState{}, 0.5, 0.5);

    CAPTURE(light.kelvin, light.tint, picked.kelvin, picked.tint);
    CHECK(std::abs(picked.kelvin - light.kelvin) < 0.01F * light.kelvin);
    CHECK(std::abs(picked.tint - light.tint) < 1.0F);
}

TEST_CASE("A pick averages a window and clamps at the edges", "[whitebalance][pick]") {
    const CameraNative cam = camera();
    const auto card = cardUnder(cam, {4500.0F, 5.0F});
    const ImageBuffer photo = photographWith(cam, {10, 10}, [&](auto, auto) { return card; });

    for (const auto [x, y] : {std::pair{0.0, 0.0}, std::pair{1.0, 1.0}, std::pair{1.0, 0.0}}) {
        const auto picked = neutralTemperatureAt(photo, DevelopState{}, x, y, 8);
        CHECK(std::abs(picked.kelvin - 4500.0F) < 50.0F);
    }
}

TEST_CASE("A pick reads the half of the picture the point is on", "[whitebalance][pick]") {
    const CameraNative cam = camera();
    const ColourTemperature warm{3200.0F, 0.0F};
    const ColourTemperature cool{8000.0F, 0.0F};
    const auto warmCard = cardUnder(cam, warm);
    const auto coolCard = cardUnder(cam, cool);

    SECTION("with no geometry, left and right of the source") {
        const ImageBuffer photo = photographWith(
            cam, {40, 20}, [&](auto x, auto) { return x < 20 ? warmCard : coolCard; });
        CHECK(std::abs(neutralTemperatureAt(photo, {}, 0.1, 0.5, 1).kelvin - 3200.0F) < 40.0F);
        CHECK(std::abs(neutralTemperatureAt(photo, {}, 0.9, 0.5, 1).kelvin - 8000.0F) < 100.0F);
    }

    SECTION("through a crop, the point is in the cropped frame") {
        const ImageBuffer photo = photographWith(
            cam, {40, 20}, [&](auto x, auto) { return x < 20 ? warmCard : coolCard; });
        DevelopState state;
        // The right 60 percent: its left edge, normalised 0, is source column 16,
        // which is warm; 0.5 is column 28, which is cool.
        state.settings.geometry.crop.rectangle = UprightCropRect{0.4, 0.0, 1.0, 1.0};
        CHECK(std::abs(neutralTemperatureAt(photo, state, 0.02, 0.5, 0).kelvin - 3200.0F) < 40.0F);
        CHECK(std::abs(neutralTemperatureAt(photo, state, 0.5, 0.5, 0).kelvin - 8000.0F) < 100.0F);
    }

    SECTION("through a quarter-turn, the top of the source is on the right") {
        // Warm on top, cool below; turned clockwise, the top is the right edge.
        const ImageBuffer photo = photographWith(
            cam, {20, 40}, [&](auto, auto y) { return y < 20 ? warmCard : coolCard; });
        DevelopState state;
        state.settings.geometry.rotation = QuarterTurn::Clockwise90;
        CHECK(std::abs(neutralTemperatureAt(photo, state, 0.9, 0.5, 1).kelvin - 3200.0F) < 40.0F);
        CHECK(std::abs(neutralTemperatureAt(photo, state, 0.1, 0.5, 1).kelvin - 8000.0F) < 100.0F);
    }

    SECTION("through a flip, the halves swap") {
        const ImageBuffer photo = photographWith(
            cam, {40, 20}, [&](auto x, auto) { return x < 20 ? warmCard : coolCard; });
        DevelopState state;
        state.settings.geometry.flipHorizontal = true;
        CHECK(std::abs(neutralTemperatureAt(photo, state, 0.1, 0.5, 1).kelvin - 8000.0F) < 100.0F);
    }

    SECTION("through the camera's own orientation") {
        ImageBuffer photo(ImageSize{40, 20}, PixelFormat::RgbaF32, cam, ImageOrientation::Rotate90);
        auto samples = photo.samples<float>();
        for (std::uint32_t y = 0; y < 20; ++y) {
            for (std::uint32_t x = 0; x < 40; ++x) {
                const auto& card = x < 20 ? warmCard : coolCard;
                const std::size_t at = (static_cast<std::size_t>(y) * 40 + x) * 4;
                samples[at] = card[0];
                samples[at + 1] = card[1];
                samples[at + 2] = card[2];
                samples[at + 3] = 1.0F;
            }
        }
        // Rotate90 turns the source clockwise, so its left is the top of the picture.
        CHECK(std::abs(neutralTemperatureAt(photo, {}, 0.5, 0.1, 1).kelvin - 3200.0F) < 40.0F);
        CHECK(std::abs(neutralTemperatureAt(photo, {}, 0.5, 0.9, 1).kelvin - 8000.0F) < 100.0F);
    }
}

TEST_CASE("A pick is refused where it cannot mean anything", "[whitebalance][pick]") {
    const CameraNative cam = camera();
    const auto card = cardUnder(cam, {5000.0F, 0.0F});
    const ImageBuffer photo = photographWith(cam, {8, 8}, [&](auto, auto) { return card; });

    SECTION("a photograph without a sensor") {
        ImageBuffer plain(ImageSize{8, 8}, PixelFormat::RgbaF32, workingEncoding);
        CHECK_THROWS_AS(neutralTemperatureAt(plain, {}, 0.5, 0.5), std::invalid_argument);
    }
    SECTION("a point outside the frame") {
        CHECK_THROWS_AS(neutralTemperatureAt(photo, {}, -0.01, 0.5), std::invalid_argument);
        CHECK_THROWS_AS(neutralTemperatureAt(photo, {}, 0.5, 1.01), std::invalid_argument);
        CHECK_THROWS_AS(
            neutralTemperatureAt(photo, {}, std::numeric_limits<double>::quiet_NaN(), 0.5),
            std::invalid_argument);
    }
    SECTION("a negative radius") {
        CHECK_THROWS_AS(neutralTemperatureAt(photo, {}, 0.5, 0.5, -1), std::invalid_argument);
    }
    SECTION("black, and a channel with nothing in it") {
        const ImageBuffer black =
            photographWith(cam, {8, 8}, [](auto, auto) { return std::array{0.0F, 0.0F, 0.0F}; });
        CHECK_THROWS_AS(neutralTemperatureAt(black, {}, 0.5, 0.5), std::invalid_argument);
        const ImageBuffer noBlue =
            photographWith(cam, {8, 8}, [](auto, auto) { return std::array{0.5F, 0.5F, 0.0F}; });
        CHECK_THROWS_AS(neutralTemperatureAt(noBlue, {}, 0.5, 0.5), std::invalid_argument);
    }
}

TEST_CASE("A picked light leaves the picked spot neutral when developed", "[whitebalance][pick]") {
    /// The end-to-end meaning (the test camera has an identity matrix): store the pick as Custom
    /// and the colour matrix the render resolves makes the card grey in the working space.
    const CameraNative cam = camera();
    const auto card = cardUnder(cam, {4200.0F, 8.0F});
    const ImageBuffer photo = photographWith(cam, {8, 8}, [&](auto, auto) { return card; });

    const auto picked = neutralTemperatureAt(photo, {}, 0.5, 0.5);
    DevelopState state;
    state.settings.color = {WhiteBalanceMode::Custom, picked.kelvin, picked.tint};
    const auto grey = colorMatrixFor(photo.encoding(), state.settings.color) * card;

    CHECK(std::abs(grey[0] - grey[1]) < 0.02F * grey[1]);
    CHECK(std::abs(grey[2] - grey[1]) < 0.02F * grey[1]);
}
