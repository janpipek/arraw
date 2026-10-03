#include <Develop.h>
#include <ImagePyramid.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <stdexcept>

using namespace arraw;
using Catch::Approx;

namespace {

/// @brief Builds a working buffer from a per-pixel generator returning four samples.
template <typename Generator> ImageBuffer made(ImageSize size, Generator&& generator) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const std::array<float, 4> pixel = generator(x, y);
            std::copy(pixel.begin(), pixel.end(), samples.begin() + (y * size.width + x) * 4);
        }
    }
    return image;
}

/// @brief Reads one sample of a buffer.
float at(const ImageBuffer& image, std::uint32_t x, std::uint32_t y, std::size_t channel) {
    return image
        .samples<float>()[(static_cast<std::size_t>(y) * image.size().width + x) * 4 + channel];
}

/// @brief A request for a long edge, never enlarging.
RenderRequest longEdge(std::uint32_t edge, Upscale upscale = Upscale::Never) {
    return {.size = RenderRequest::FitInside{edge, edge}, .upscale = upscale};
}

} // namespace

TEST_CASE("Halving averages 2x2 blocks exactly", "[pyramid]") {
    SECTION("an even size") {
        // Red is x + 4y, so each block's mean is easy to say: its top-left + 2.5.
        const ImageBuffer source = made({4, 4}, [](std::uint32_t x, std::uint32_t y) {
            return std::array<float, 4>{static_cast<float>(x + 4 * y), 1.0F, -2.0F, 1.0F};
        });
        const ImageBuffer half = halved(source);
        REQUIRE(half.size() == ImageSize{2, 2});
        REQUIRE(at(half, 0, 0, 0) == 2.5F);
        REQUIRE(at(half, 1, 0, 0) == 4.5F);
        REQUIRE(at(half, 0, 1, 0) == 10.5F);
        REQUIRE(at(half, 1, 1, 0) == 12.5F);
        REQUIRE(at(half, 1, 1, 1) == 1.0F);
        REQUIRE(at(half, 1, 1, 2) == -2.0F);
        REQUIRE(at(half, 1, 1, 3) == 1.0F);
    }
    SECTION("an odd size averages the samples an edge block has") {
        const ImageBuffer source = made({5, 3}, [](std::uint32_t x, std::uint32_t y) {
            return std::array<float, 4>{static_cast<float>(x + 5 * y), 0.0F, 0.0F, 1.0F};
        });
        const ImageBuffer half = halved(source);
        REQUIRE(half.size() == ImageSize{3, 2});
        // Interior block: samples 0, 1, 5, 6.
        REQUIRE(at(half, 0, 0, 0) == 3.0F);
        // Last column of the first row pair: samples 4 and 9.
        REQUIRE(at(half, 2, 0, 0) == 6.5F);
        // Last row of the first column pair: samples 10 and 11.
        REQUIRE(at(half, 0, 1, 0) == 10.5F);
        // The corner: sample 14 alone.
        REQUIRE(at(half, 2, 1, 0) == 14.0F);
        REQUIRE(at(half, 2, 1, 3) == 1.0F);
    }
    SECTION("a single row or column halves along the other side") {
        const ImageBuffer source = made({5, 1}, [](std::uint32_t x, std::uint32_t) {
            return std::array<float, 4>{static_cast<float>(x), 0.0F, 0.0F, 1.0F};
        });
        const ImageBuffer half = halved(source);
        REQUIRE(half.size() == ImageSize{3, 1});
        REQUIRE(at(half, 0, 0, 0) == 0.5F);
        REQUIRE(at(half, 2, 0, 0) == 4.0F);
    }
}

TEST_CASE("Halving keeps encoding and orientation and returns working floats", "[pyramid]") {
    ImageBuffer source(ImageSize{6, 4}, PixelFormat::RgbU8, NamedEncoding::Srgb,
                       ImageOrientation::Rotate90);
    auto samples = source.samples<std::uint8_t>();
    std::fill(samples.begin(), samples.end(), std::uint8_t{255});

    const ImageBuffer half = halved(source);

    REQUIRE(half.format() == PixelFormat::RgbaF32);
    REQUIRE(half.size() == ImageSize{3, 2});
    REQUIRE(half.orientation() == ImageOrientation::Rotate90);
    REQUIRE(half.encoding() == source.encoding());
    REQUIRE(at(half, 1, 1, 0) == 1.0F);
    REQUIRE(at(half, 1, 1, 3) == 1.0F);
}

TEST_CASE("Halving an image of one pixel is refused", "[pyramid]") {
    const ImageBuffer single = made({1, 1}, [](std::uint32_t, std::uint32_t) {
        return std::array<float, 4>{0.5F, 0.5F, 0.5F, 1.0F};
    });
    REQUIRE_THROWS_AS(halved(single), std::invalid_argument);
}

TEST_CASE("A transparent pixel does not darken its neighbours when halved", "[pyramid]") {
    // Left pixel of each pair: opaque white. Right: transparent, but "black".
    const ImageBuffer source = made({2, 2}, [](std::uint32_t x, std::uint32_t) {
        return x == 0 ? std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F}
                      : std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F};
    });
    const ImageBuffer half = halved(source);
    REQUIRE(at(half, 0, 0, 0) == Approx(1.0F));
    REQUIRE(at(half, 0, 0, 3) == Approx(0.5F));

    SECTION("a block with no alpha at all is transparent black") {
        const ImageBuffer clear = made({2, 2}, [](std::uint32_t, std::uint32_t) {
            return std::array<float, 4>{0.7F, 0.7F, 0.7F, 0.0F};
        });
        const ImageBuffer cleared = halved(clear);
        for (std::size_t channel = 0; channel < 4; ++channel) {
            REQUIRE(at(cleared, 0, 0, channel) == 0.0F);
        }
    }
}

TEST_CASE("The pyramid level a render needs", "[pyramid]") {
    const ImageSize source{6000, 4000};
    const DevelopState plain;
    const auto level = [&](const DevelopState& state, const RenderRequest& request,
                           ImageSize size = {6000, 4000},
                           ImageOrientation orientation = ImageOrientation::Normal) {
        return pyramidLevelFor(size, orientation, state, request);
    };

    SECTION("the smallest level that still covers the output") {
        REQUIRE(level(plain, longEdge(1500)) == 2);
        REQUIRE(level(plain, longEdge(1501)) == 1);
        REQUIRE(level(plain, longEdge(3000)) == 1);
        REQUIRE(level(plain, longEdge(3001)) == 0);
    }
    SECTION("the short side binds too") {
        // 6000x4000 into 6000x500 is 750x500, which level 3 is exactly; the
        // width alone would allow no reduction at all.
        REQUIRE(level(plain, {.size = RenderRequest::FitInside{6000, 500}}) == 3);
    }
    SECTION("no size, a scale of one, or a larger request needs full detail") {
        REQUIRE(level(plain, {}) == 0);
        REQUIRE(level(plain, {.size = RenderRequest::Scale{1.0}}) == 0);
        REQUIRE(level(plain, longEdge(9000)) == 0);
    }
    SECTION("a scale works like a box") {
        REQUIRE(level(plain, {.size = RenderRequest::Scale{0.25}}) == 2);
    }
    SECTION("upscaling is level 0 whatever the size") {
        REQUIRE(level(plain, longEdge(100, Upscale::Allowed)) == 0);
    }
    SECTION("a crop lowers the level, as it leaves fewer pixels") {
        DevelopState cropped;
        cropped.settings.geometry.crop.rectangle =
            UprightCropRect{.left = 0.0, .top = 0.0, .right = 0.5, .bottom = 1.0};
        // 3000x4000 into a 1500 box: 1125x1500, and level 1 is 1500x2000.
        REQUIRE(level(cropped, longEdge(1500)) == 1);
    }
    SECTION("a quarter turn swaps the axes") {
        DevelopState turned;
        turned.settings.geometry.rotation = QuarterTurn::Clockwise90;
        REQUIRE(level(turned, {.size = RenderRequest::FitInside{1500, 2250}}) == 1);
        // Unturned, the same box would bind on the other side.
        REQUIRE(level(plain, {.size = RenderRequest::FitInside{1500, 2250}}) == 2);
    }
    SECTION("a camera orientation swaps them too") {
        REQUIRE(level(plain, {.size = RenderRequest::FitInside{1500, 2250}}, source,
                      ImageOrientation::Rotate90) == 1);
    }
    SECTION("a region needs the level that still covers the output over the region") {
        // The whole frame at a 750 box is 750x500, level 3. Half of each side
        // is 3000x2000 to fit the same box, so one level finer.
        const RenderRequest::FitInside box{750, 750};
        REQUIRE(level(plain, {.size = box}) == 3);
        REQUIRE(level(plain, {.size = box, .region = RenderRequest::Region{0.0, 0.0, 0.5, 0.5}}) ==
                2);
        REQUIRE(level(plain, {.size = box, .region = RenderRequest::Region{0.5, 0.5, 1.0, 1.0}}) ==
                2);
        // A region covering the frame changes nothing, nor does one that is
        // already smaller than the box.
        REQUIRE(level(plain, {.size = box, .region = RenderRequest::Region{0.0, 0.0, 1.0, 1.0}}) ==
                3);
        REQUIRE(level(plain, {.size = box, .region = RenderRequest::Region{0.0, 0.0, 0.1, 0.1}}) ==
                0);
        REQUIRE_THROWS_AS(level(plain, {.region = RenderRequest::Region{0.5, 0.0, 0.5, 1.0}}),
                          std::invalid_argument);
    }
    SECTION("a tiny image never loops") {
        REQUIRE(level(plain, longEdge(1), {1, 1}) == 0);
        REQUIRE(level(plain, longEdge(1), {1, 64}) == 6);
    }
    SECTION("what cannot be resolved throws") {
        REQUIRE_THROWS_AS(level(plain, {.size = RenderRequest::FitInside{0, 10}}),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(level(plain, longEdge(10), {0, 0}), std::invalid_argument);
    }
}

TEST_CASE("Developing a reduced level stays near developing the full photograph",
          "[pyramid][develop]") {
    // Smooth gradients, a soft wave and a little fine detail: the stuff a
    // photograph is made of, without the one-pixel edges only a test chart has.
    const ImageBuffer full = made({1200, 800}, [](std::uint32_t x, std::uint32_t y) {
        const float u = static_cast<float>(x) / 1200.0F;
        const float v = static_cast<float>(y) / 800.0F;
        const float wave = 0.5F + 0.5F * std::sin(2.0F * std::numbers::pi_v<float> * 9.0F * u) *
                                      std::sin(2.0F * std::numbers::pi_v<float> * 5.0F * v);
        const float fine =
            0.5F + 0.5F * std::sin(static_cast<float>(x) * 1.3F + static_cast<float>(y) * 0.7F);
        return std::array<float, 4>{0.05F + 0.8F * u * wave + 0.05F * fine,
                                    0.05F + 0.8F * v * wave + 0.05F * fine,
                                    0.05F + 0.6F * (1.0F - u) * (1.0F - v) + 0.05F * fine, 1.0F};
    });
    DevelopState state;
    state.settings.tone = {.exposure = 0.4F,
                           .contrast = 30.0F,
                           .shadows = 30.0F,
                           .highlights = -30.0F,
                           .blacks = 10.0F,
                           .whites = 20.0F,
                           .filmicHighlights = 70.0F};
    const RenderRequest request = longEdge(300);

    const int level = pyramidLevelFor(full.size(), full.orientation(), state, request);
    REQUIRE(level == 2);
    ImageBuffer reduced = halved(halved(full));

    const ImageBuffer exact = develop(full, state, request);
    const ImageBuffer preview = develop(reduced, state, request);
    REQUIRE(preview.size() == exact.size());

    double sum = 0.0;
    double worst = 0.0;
    const auto want = exact.samples<float>();
    const auto got = preview.samples<float>();
    for (std::size_t i = 0; i < want.size(); ++i) {
        const double difference = std::abs(static_cast<double>(want[i]) - got[i]);
        sum += difference;
        worst = std::max(worst, difference);
    }
    const double mean = sum / static_cast<double>(want.size());
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "pyramid fidelity: mean %.3g, max %.3g\n", mean, worst);
    }
    CAPTURE(mean, worst);
    // Tone applied to averaged pixels is not tone averaged (ADR 020): a
    // deliberate, accepted difference, and far looser than CPU/GPU parity.
    // Measured on this image: mean 0.0024, max 0.011 (linear working units, one
    // is white). The bounds are about twice that: tight enough to catch a
    // shifted or mis-averaged level, which is off by whole percents.
    REQUIRE(mean <= 0.005);
    REQUIRE(worst <= 0.02);
}
