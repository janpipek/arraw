#include "ProcessingPlan.h"
#include "support/TestImages.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <ImageBuffer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

using namespace arraw;

namespace {

/// @brief Settings under which the pointwise chain leaves a working-space image as it is.
DevelopSettings plainSettings() {
    DevelopSettings settings;
    settings.tone = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    return settings;
}

ImageBuffer frameOf(ImageSize size) {
    return test::rainbow(size, PixelFormat::RgbaF32, workingEncoding);
}

/// @brief Copies a rectangle out of a working buffer, the long way round.
ImageBuffer cut(const ImageBuffer& image, std::uint32_t x, std::uint32_t y, ImageSize size) {
    ImageBuffer result(size, workingFormat, workingEncoding);
    const auto in = image.samples<float>();
    const auto out = result.samples<float>();
    for (std::uint32_t row = 0; row < size.height; ++row) {
        for (std::uint32_t column = 0; column < size.width; ++column) {
            for (std::size_t c = 0; c < 4; ++c) {
                out[(static_cast<std::size_t>(row) * size.width + column) * 4 + c] =
                    in[(static_cast<std::size_t>(y + row) * image.size().width + x + column) * 4 +
                       c];
            }
        }
    }
    return result;
}

void requireIdentical(const ImageBuffer& expected, const ImageBuffer& actual) {
    REQUIRE(actual.size() == expected.size());
    REQUIRE(std::ranges::equal(actual.bytes(), expected.bytes()));
}

RenderRequest withRegion(RenderRequest::Region region,
                         std::optional<std::variant<RenderRequest::FitInside, RenderRequest::Scale>>
                             size = std::nullopt) {
    return {.size = size, .region = region};
}

} // namespace

TEST_CASE("A region covering the frame is the render of no region", "[region]") {
    const ImageBuffer source = frameOf({48, 32});
    const DevelopState state{plainSettings()};
    const RenderRequest::Region whole{0.0, 0.0, 1.0, 1.0};

    SECTION("at the photograph's own size") {
        requireIdentical(develop(source, state), develop(source, state, withRegion(whole)));
    }
    SECTION("resized") {
        const RenderRequest::FitInside box{20, 20};
        requireIdentical(develop(source, state, {.size = box}),
                         develop(source, state, withRegion(whole, box)));
        const auto plain = planFor(source, state, {.size = box});
        const auto framed = planFor(source, state, withRegion(whole, box));
        REQUIRE(plain.resize == framed.resize);
    }
}

TEST_CASE("A region snaps outward to whole pixels, at least one", "[region]") {
    const ImageSize frame{40, 30};

    const auto aligned = arraw::regionOf(withRegion({0.25, 0.5, 0.75, 1.0}), frame);
    REQUIRE(aligned == PixelRegion{10, 15, 20, 15});

    const auto outward = arraw::regionOf(withRegion({0.26, 0.51, 0.74, 0.99}), frame);
    REQUIRE(outward == PixelRegion{10, 15, 20, 15});

    const auto sliver = arraw::regionOf(withRegion({0.5, 0.5, 0.5001, 0.5001}), frame);
    REQUIRE(sliver.width == 1);
    REQUIRE(sliver.height == 1);

    const auto corner = arraw::regionOf(withRegion({0.9999, 0.9999, 1.0, 1.0}), frame);
    REQUIRE(corner == PixelRegion{39, 29, 1, 1});
}

TEST_CASE("A region at the photograph's resolution is the pixels cut from the full render",
          "[region]") {
    const ImageBuffer source = frameOf({48, 32});
    DevelopSettings settings = plainSettings();
    settings.tone.exposure = 0.5F;
    settings.geometry.straighten = 4.0;
    const DevelopState state{settings};

    // Cut and kept, so exact: pointwise and geometry are untouched, and the cut
    // is a copy.
    const ImageBuffer full = develop(source, state);
    const ImageBuffer part = develop(source, state, withRegion({0.25, 0.5, 0.75, 1.0}));
    const PixelRegion pixels = arraw::regionOf(withRegion({0.25, 0.5, 0.75, 1.0}), full.size());
    requireIdentical(cut(full, pixels.x, pixels.y, pixels.size()), part);
}

TEST_CASE("A resized region is what resizing the cut frame gives", "[region]") {
    // Exactly comparable where nothing before the resize touches the pixels: a
    // working-space source under plain settings and an identity geometry. The
    // region render must then equal developing the cut-out image on its own,
    // since the resize clamps at the region's edges rather than reading past
    // them.
    const ImageBuffer source = frameOf({64, 48});
    const DevelopState state{plainSettings()};
    const RenderRequest::Region region{0.25, 0.25, 0.75, 1.0};
    const PixelRegion pixels = arraw::regionOf(withRegion(region), source.size());
    REQUIRE(pixels == PixelRegion{16, 12, 32, 36});
    const ImageBuffer cutOut = cut(source, pixels.x, pixels.y, pixels.size());

    for (const ResizeFilter filter : {ResizeFilter::Lanczos3, ResizeFilter::Bilinear}) {
        for (const auto& [size, upscale] :
             {std::pair<std::variant<RenderRequest::FitInside, RenderRequest::Scale>, Upscale>{
                  RenderRequest::FitInside{16, 16}, Upscale::Never},
              {RenderRequest::Scale{0.3}, Upscale::Never},
              {RenderRequest::FitInside{100, 100}, Upscale::Allowed}}) {
            const RenderRequest request{.size = size, .upscale = upscale, .filter = filter};
            RenderRequest regional = request;
            regional.region = region;
            requireIdentical(develop(cutOut, state, request), develop(source, state, regional));
        }
    }
}

TEST_CASE("The size resolves against the region", "[region]") {
    const ImageBuffer source = frameOf({64, 48});
    const DevelopState state{plainSettings()};
    const auto request = withRegion({0.0, 0.0, 0.5, 0.5}, RenderRequest::FitInside{100, 100});

    // The region is 32x24: without upscaling it is kept, with it, it fills the box.
    REQUIRE(develop(source, state, request).size() == ImageSize{32, 24});
    RenderRequest enlarging = request;
    enlarging.upscale = Upscale::Allowed;
    REQUIRE(develop(source, state, enlarging).size() == ImageSize{100, 75});
}

TEST_CASE("A region that is not well formed is refused", "[region]") {
    const ImageBuffer source = frameOf({16, 16});
    const DevelopState state{plainSettings()};
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();

    const RenderRequest::Region bad[] = {
        {0.5, 0.0, 0.5, 1.0},  // No width.
        {0.6, 0.0, 0.4, 1.0},  // Inverted.
        {0.0, 0.5, 1.0, 0.5},  // No height.
        {-0.1, 0.0, 1.0, 1.0}, // Outside.
        {0.0, 0.0, 1.1, 1.0},  {0.0, -0.5, 1.0, 1.0}, {0.0, 0.0, 1.0, 1.5}, {nan, 0.0, 1.0, 1.0},
        {0.0, 0.0, inf, 1.0},  {0.0, nan, 1.0, 1.0},  {0.0, 0.0, 1.0, nan},
    };
    for (const RenderRequest::Region& region : bad) {
        CAPTURE(region.left, region.top, region.right, region.bottom);
        REQUIRE_THROWS_AS(develop(source, state, withRegion(region)), std::invalid_argument);
        REQUIRE_THROWS_AS(planFor(source, state, withRegion(region)), std::invalid_argument);
    }
}

TEST_CASE("A region belongs to the resize stage of the plan", "[region][plan]") {
    const ImageBuffer source = frameOf({64, 48});
    DevelopSettings settings = plainSettings();
    settings.geometry.straighten = 3.0;
    const DevelopState state{settings};
    const RenderRequest::FitInside box{20, 20};

    const auto left = planFor(source, state, withRegion({0.0, 0.0, 0.5, 0.5}, box));
    const auto right = planFor(source, state, withRegion({0.5, 0.0, 1.0, 0.5}, box));
    const auto again = planFor(source, state, withRegion({0.0, 0.0, 0.5, 0.5}, box));

    REQUIRE_FALSE(left.resize->region == right.resize->region);
    REQUIRE(prefixMatches(left, right, Stage::Pointwise));
    REQUIRE(prefixMatches(left, right, Stage::Geometry));
    REQUIRE_FALSE(prefixMatches(left, right, Stage::Resize));
    REQUIRE(prefixMatches(left, again, Stage::Resize));
    REQUIRE_FALSE(left == right);
}

TEST_CASE("A geometry checkpoint is resumed across regions", "[region][checkpoint]") {
    const ImageBuffer source = frameOf({64, 48});
    DevelopSettings settings = plainSettings();
    settings.tone.exposure = 0.3F;
    settings.geometry.straighten = 3.0;
    const DevelopState state{settings};

    const RenderCheckpoint geometry = developUntil(source, state, Stage::Geometry);
    for (const RenderRequest::Region region : {RenderRequest::Region{0.0, 0.0, 0.5, 0.5},
                                               {0.5, 0.25, 1.0, 0.75},
                                               {0.1, 0.1, 0.9, 0.9}}) {
        const auto request = withRegion(region, RenderRequest::FitInside{20, 20});
        const RenderCheckpoint resumed =
            resumeFrom(geometry, source, state, Stage::Resize, request);
        REQUIRE(resumed.boundary() == Stage::Resize);
        requireIdentical(develop(source, state, request), resumed.readBack());
    }
}

TEST_CASE("A region given as fractions of whole pixels renders exactly those pixels", "[region]") {
    // 1/3-style fractions do not survive the division exactly; the snap must
    // not push such an edge out by a pixel.
    const ImageSize frame{3000, 2000};
    for (const std::uint32_t x : {1U, 7U, 1001U, 2999U}) {
        RenderRequest request;
        request.region = RenderRequest::Region{
            .left = x / 3000.0, .top = 0.0, .right = (x + 1) / 3000.0, .bottom = 1.0};
        const RenderRequest::Region rendered = renderedRegion(request, frame);
        CAPTURE(x);
        CHECK(rendered.left * 3000.0 == Catch::Approx(x).margin(1e-9));
        CHECK(rendered.right * 3000.0 == Catch::Approx(x + 1).margin(1e-9));
    }
}

TEST_CASE("The rendered region is the requested one snapped outward to the frame's pixels",
          "[region]") {
    RenderRequest request;
    request.region = RenderRequest::Region{.left = 0.3, .top = 0.25, .right = 0.55, .bottom = 0.5};
    // 0.3 * 10 = 3 and 0.55 * 10 = 5.5: the far edge goes out to 6.
    const RenderRequest::Region rendered = renderedRegion(request, ImageSize{10, 4});
    CHECK(rendered.left == Catch::Approx(0.3));
    CHECK(rendered.right == Catch::Approx(0.6));
    CHECK(rendered.top == Catch::Approx(0.25));
    CHECK(rendered.bottom == Catch::Approx(0.5));
    // No region renders the whole frame.
    const RenderRequest::Region whole = renderedRegion(RenderRequest{}, ImageSize{10, 4});
    CHECK((whole.left == 0.0 && whole.top == 0.0 && whole.right == 1.0 && whole.bottom == 1.0));
}
