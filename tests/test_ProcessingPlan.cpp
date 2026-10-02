#include "ProcessingPlan.h"
#include "support/Fixtures.h"
#include "support/TestImages.h"

#include <Develop.h>
#include <ImageImport.h>
#include <Photo.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

TEST_CASE("The plan resolves a request into its resize block", "[plan][resample]") {
    const ImageBuffer source({60, 40}, workingFormat, workingEncoding);
    const DevelopSettings settings;

    const auto none = planFor(source, settings);
    REQUIRE(none.resize.has_value());
    REQUIRE(none.resize->outputSize == ImageSize{60, 40});
    REQUIRE(none.resize->isIdentity(none.geometry->outputSize));

    const auto box =
        planFor(source, settings,
                {.size = RenderRequest::FitInside{30, 100}, .filter = ResizeFilter::Bilinear});
    REQUIRE(box.resize->outputSize == ImageSize{30, 20});
    REQUIRE(box.resize->filter == ResizeFilter::Bilinear);

    // The crop comes first: a size is resolved against what is left of it.
    DevelopSettings cropped;
    cropped.geometry.crop.aspect = CropRatio{1};
    const auto square = planFor(source, cropped, {.size = RenderRequest::Scale{0.5}});
    REQUIRE(square.resize->outputSize == ImageSize{20, 20});

    REQUIRE(planFor(source, settings, {.size = RenderRequest::Scale{4.0}}).resize->outputSize ==
            ImageSize{60, 40});
    REQUIRE(
        planFor(source, settings, {.size = RenderRequest::Scale{4.0}, .upscale = Upscale::Allowed})
            .resize->outputSize == ImageSize{240, 160});
    REQUIRE_THROWS_AS(planFor(source, settings, {.size = RenderRequest::Scale{0.0}}),
                      std::invalid_argument);

    // The narrow overload resolves colour and tone only.
    REQUIRE_FALSE(planFor(ColorEncoding{workingEncoding}, settings).resize.has_value());
}

TEST_CASE("The plan knows whether the pixels are opaque", "[plan][resample]") {
    const RenderRequest shrink{.size = RenderRequest::Scale{0.5}};
    const DevelopSettings settings;
    const auto opaque = [&](const ImageBuffer& source) {
        return planFor(source, settings, shrink).resize->opaque;
    };

    // No alpha channel is opaque without a look.
    REQUIRE(opaque(ImageBuffer({8, 8}, PixelFormat::RgbU8, workingEncoding)));
    REQUIRE(opaque(ImageBuffer({8, 8}, PixelFormat::RgbU16, workingEncoding)));
    REQUIRE(opaque(ImageBuffer({8, 8}, PixelFormat::RgbF32, workingEncoding)));

    // With one, every sample must be exactly one.
    ImageBuffer u8({8, 8}, PixelFormat::RgbaU8, workingEncoding);
    ImageBuffer u16({8, 8}, PixelFormat::RgbaU16, workingEncoding);
    ImageBuffer f32({8, 8}, PixelFormat::RgbaF32, workingEncoding);
    REQUIRE_FALSE(opaque(u8)); // Zero-filled: transparent.
    std::ranges::fill(u8.samples<std::uint8_t>(), std::uint8_t{255});
    std::ranges::fill(u16.samples<std::uint16_t>(), std::uint16_t{65535});
    std::ranges::fill(f32.samples<float>(), 1.0F);
    REQUIRE(opaque(u8));
    REQUIRE(opaque(u16));
    REQUIRE(opaque(f32));

    // The last sample alone decides, and so does the smallest step from one.
    u8.samples<std::uint8_t>()[8 * 8 * 4 - 1] = 254;
    u16.samples<std::uint16_t>()[8 * 8 * 4 - 1] = 65534;
    f32.samples<float>()[8 * 8 * 4 - 1] = std::nextafter(1.0F, 0.0F);
    REQUIRE_FALSE(opaque(u8));
    REQUIRE_FALSE(opaque(u16));
    REQUIRE_FALSE(opaque(f32));
    f32.samples<float>()[8 * 8 * 4 - 1] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(opaque(f32));
    f32.samples<float>()[8 * 8 * 4 - 1] = 1.0F;
    f32.samples<float>()[3] = 1.5F; // Above one is not opaque either.
    REQUIRE_FALSE(opaque(f32));

    // Nothing is claimed where no resize runs, or where there are no pixels.
    const ImageBuffer rgb({8, 8}, PixelFormat::RgbU8, workingEncoding);
    REQUIRE_FALSE(planFor(rgb, settings).resize->opaque);
    const auto photo = openPhoto(test::fixture(skewedFixture));
    const auto fromPhoto = planFor(photo, {.size = RenderRequest::Scale{0.5}});
    REQUIRE_FALSE(fromPhoto.resize->isIdentity(fromPhoto.geometry->outputSize));
    REQUIRE_FALSE(fromPhoto.resize->opaque);
}

TEST_CASE("Whether the pixels are opaque does not make two resize plans differ",
          "[plan][resample]") {
    // The flag is a hint for execution: a plan made without pixels, or from a
    // source with one translucent sample, is the same render.
    const DevelopSettings settings;
    const RenderRequest request{.size = RenderRequest::Scale{0.5}};
    ImageBuffer opaqueSource({8, 8}, workingFormat, workingEncoding);
    for (std::size_t i = 3; i < opaqueSource.samples<float>().size(); i += 4) {
        opaqueSource.samples<float>()[i] = 1.0F;
    }
    ImageBuffer translucent = opaqueSource.clone();
    translucent.samples<float>()[3] = 0.5F;

    const auto fast = planFor(opaqueSource, settings, request);
    const auto general = planFor(translucent, settings, request);
    REQUIRE(fast.resize->opaque);
    REQUIRE_FALSE(general.resize->opaque);
    REQUIRE(fast.resize == general.resize);
    REQUIRE(fast == general);

    // Size and filter still tell plans apart.
    REQUIRE_FALSE(fast == planFor(opaqueSource, settings, {.size = RenderRequest::Scale{0.25}}));
    REQUIRE_FALSE(fast ==
                  planFor(opaqueSource, settings,
                          {.size = RenderRequest::Scale{0.5}, .filter = ResizeFilter::Bilinear}));
}
