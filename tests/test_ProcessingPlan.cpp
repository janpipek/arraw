#include "ProcessingPlan.h"

#include "Develop.h"
#include "ImageImport.h"

#include "support/Fixtures.h"
#include "support/TestImages.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <stdexcept>
#include <string_view>
#include <variant>

using namespace arraw;

namespace {

constexpr std::string_view skewedFixture = "linear-32x24-skewed.dng";

} // namespace

TEST_CASE("A plan with nothing set leaves a colour alone", "[plan]") {
    constexpr Colour colour{0.25F, 0.5F, 0.75F};

    REQUIRE(developPixel({}, colour) == colour);
}

TEST_CASE("The plan carries exposure as a gain, not as stops", "[plan]") {
    /// Settings are what a photographer sets; a plan is what the pixels need.
    /// Resolving 2^EV once per photograph keeps the per-pixel chain to a
    /// multiply, and is the shape the GPU's uniform block wants (ADR 011).
    const auto plan = planFor(ColorEncoding{workingEncoding}, {.tone = {.exposure = 2.0F}});

    REQUIRE(std::abs(plan.exposureGain - 4.0F) < 1e-6F);
    REQUIRE(plan.toWorking == Matrix3::identity());

    const Colour developed = developPixel(plan, {0.1F, 0.2F, 0.3F});
    REQUIRE(std::abs(developed[0] - 0.4F) < 1e-6F);
    REQUIRE(std::abs(developed[2] - 1.2F) < 1e-6F);
}

TEST_CASE("White balance is folded into the plan's one transform", "[plan]") {
    /// White balance, the camera matrix and the change of primaries are all
    /// linear, so they arrive as a single matrix rather than as three passes.
    const auto source = loadImage(test::fixture(skewedFixture));
    const auto* camera = std::get_if<CameraNative>(&source.encoding());
    REQUIRE(camera != nullptr);

    const auto asShot = planFor(source.encoding(), {});
    const auto custom =
        planFor(source.encoding(), {.color = {.whiteBalance = WhiteBalanceMode::Custom,
                                              .temperature = 4000.0F,
                                              .tint = 0.0F}});

    REQUIRE(asShot.toWorking == camera->toWorking);
    REQUIRE_FALSE(custom.toWorking == camera->toWorking);
    REQUIRE(custom.exposureGain == asShot.exposureGain);
}

TEST_CASE("A plan refuses what development cannot start from", "[plan]") {
    REQUIRE_THROWS_AS(planFor(ColorEncoding{NamedEncoding::Srgb}, {}), std::invalid_argument);
    REQUIRE_THROWS_AS(
        planFor(ColorEncoding{workingEncoding},
                {.color = {.whiteBalance = WhiteBalanceMode::Custom, .temperature = 4000.0F}}),
        std::invalid_argument);
}

TEST_CASE("Developing a buffer is the chain applied to each pixel", "[plan]") {
    /// The traversal must add nothing of its own: what a buffer comes back as
    /// has to be what developPixel says, pixel by pixel. That is what lets the
    /// order be tested without a buffer at all.
    const auto source = test::rainbow({4, 3}, PixelFormat::RgbaU16, workingEncoding);
    const DevelopSettings settings{.tone = {.exposure = -1.0F}};

    const auto developed = develop(source, settings);
    const auto plan = planFor(source.encoding(), settings);

    const auto before = source.samples<std::uint16_t>();
    const auto after = developed.samples<float>();
    for (std::size_t pixel = 0; pixel < 12; ++pixel) {
        const Colour input{static_cast<float>(before[pixel * 4]) / 65535.0F,
                           static_cast<float>(before[pixel * 4 + 1]) / 65535.0F,
                           static_cast<float>(before[pixel * 4 + 2]) / 65535.0F};
        const Colour expected = developPixel(plan, input);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            REQUIRE(std::abs(after[pixel * 4 + channel] - expected[channel]) < 1e-6F);
        }
    }
}
