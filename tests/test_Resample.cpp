#include "ProcessingPlan.h"
#include "Resample.h"

#include <Develop.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace arraw;

namespace {

constexpr float quietNan = std::numeric_limits<float>::quiet_NaN();

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

/// @brief Largest deviation from a value over the pixels at least a margin inside the edges.
float interiorDeviation(const ImageBuffer& image, float expected, std::uint32_t margin) {
    float worst = 0.0F;
    for (std::uint32_t y = margin; y + margin < image.size().height; ++y) {
        for (std::uint32_t x = margin; x + margin < image.size().width; ++x) {
            worst = std::max(worst, std::abs(at(image, x, y, 0) - expected));
        }
    }
    return worst;
}

constexpr ResizeFilter filters[] = {ResizeFilter::Lanczos3, ResizeFilter::Bilinear};

} // namespace

TEST_CASE("Resolved sizes follow the request", "[resample][size]") {
    const ImageSize landscape{6000, 4000};
    const ImageSize portrait{4000, 6000};
    const auto fit = [](std::uint32_t w, std::uint32_t h, Upscale up = Upscale::Never) {
        return RenderRequest{.size = RenderRequest::FitInside{w, h}, .upscale = up};
    };
    const auto scaled = [](double factor, Upscale up = Upscale::Never) {
        return RenderRequest{.size = RenderRequest::Scale{factor}, .upscale = up};
    };

    SECTION("no size keeps the cropped size") {
        REQUIRE(resolvedSize({}, landscape) == landscape);
    }
    SECTION("a long edge fits landscape and portrait alike") {
        REQUIRE(resolvedSize(fit(1500, 1500), landscape) == ImageSize{1500, 1000});
        REQUIRE(resolvedSize(fit(1500, 1500), portrait) == ImageSize{1000, 1500});
    }
    SECTION("a box fits on the side that binds") {
        REQUIRE(resolvedSize(fit(3000, 1000), landscape) == ImageSize{1500, 1000});
        REQUIRE(resolvedSize(fit(1000, 3000), landscape) == ImageSize{1000, 667});
    }
    SECTION("a scale multiplies both sides") {
        REQUIRE(resolvedSize(scaled(0.5), landscape) == ImageSize{3000, 2000});
        REQUIRE(resolvedSize(scaled(2.0, Upscale::Allowed), landscape) == ImageSize{12000, 8000});
    }
    SECTION("upscaling is capped unless allowed") {
        REQUIRE(resolvedSize(scaled(2.0), landscape) == landscape);
        REQUIRE(resolvedSize(fit(12000, 12000), landscape) == landscape);
        REQUIRE(resolvedSize(fit(12000, 12000, Upscale::Allowed), landscape) ==
                ImageSize{12000, 8000});
        REQUIRE(resolvedSize(fit(6000, 4000), landscape) == landscape);
    }
    SECTION("each side rounds to nearest") {
        REQUIRE(resolvedSize(scaled(0.3333), ImageSize{10, 10}) == ImageSize{3, 3});
        REQUIRE(resolvedSize(scaled(0.35), ImageSize{10, 11}) == ImageSize{4, 4});
        REQUIRE(resolvedSize(fit(100, 100), ImageSize{300, 200}) == ImageSize{100, 67});
    }
    SECTION("a side is at least one pixel") {
        REQUIRE(resolvedSize(scaled(0.001), landscape) == ImageSize{6, 4});
        REQUIRE(resolvedSize(fit(10, 10), ImageSize{10000, 3}) == ImageSize{10, 1});
        REQUIRE(resolvedSize(scaled(0.01), ImageSize{10, 10}) == ImageSize{1, 1});
    }
    SECTION("a box is never exceeded") {
        const auto size = resolvedSize(fit(1999, 1333), ImageSize{6001, 4003});
        REQUIRE(size.width <= 1999);
        REQUIRE(size.height <= 1333);
    }
    SECTION("invalid requests throw") {
        REQUIRE_THROWS_AS(resolvedSize(fit(0, 100), landscape), std::invalid_argument);
        REQUIRE_THROWS_AS(resolvedSize(fit(100, 0), landscape), std::invalid_argument);
        REQUIRE_THROWS_AS(resolvedSize(scaled(0.0), landscape), std::invalid_argument);
        REQUIRE_THROWS_AS(resolvedSize(scaled(-1.0), landscape), std::invalid_argument);
        REQUIRE_THROWS_AS(resolvedSize(scaled(quietNan), landscape), std::invalid_argument);
        REQUIRE_THROWS_AS(resolvedSize(scaled(std::numeric_limits<double>::infinity()), landscape),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(resolvedSize(scaled(1e12, Upscale::Allowed), landscape),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(resolvedSize({}, ImageSize{0, 5}), std::invalid_argument);
    }
}

TEST_CASE("A constant image stays constant, so the weights sum to 1", "[resample]") {
    const auto filter = GENERATE(ResizeFilter::Lanczos3, ResizeFilter::Bilinear);
    const double factor = GENERATE(0.5, 0.37, 0.125, 1.5, 3.1);
    const ImageSize source{23, 17};
    const ImageSize target{
        static_cast<std::uint32_t>(std::max(1.0, std::round(source.width * factor))),
        static_cast<std::uint32_t>(std::max(1.0, std::round(source.height * factor)))};

    const auto result = resample(
        made(source, [](auto, auto) { return std::array<float, 4>{0.3F, 0.6F, 0.9F, 1.0F}; }),
        target, filter);

    REQUIRE(result.size() == target);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            REQUIRE(at(result, x, y, 0) == Catch::Approx(0.3F).margin(1e-6));
            REQUIRE(at(result, x, y, 1) == Catch::Approx(0.6F).margin(1e-6));
            REQUIRE(at(result, x, y, 2) == Catch::Approx(0.9F).margin(1e-6));
            REQUIRE(at(result, x, y, 3) == Catch::Approx(1.0F).margin(1e-6));
        }
    }
}

TEST_CASE("Shrinking fine stripes 8x gives grey, not alias", "[resample][alias]") {
    /// One-pixel stripes are the worst case: at 1/8 scale their true mean is
    /// 0.5 and everything else is alias. A kernel that is not widened by
    /// 1/scale would leave a large residual at the inexact 35-px size (at an
    /// exact 8x symmetry hides it). The margin skips the border, where edge
    /// extension legitimately repeats half a period.
    constexpr float bound = 0.05F;
    const auto filter = GENERATE(ResizeFilter::Lanczos3, ResizeFilter::Bilinear);
    const auto direction = GENERATE(0, 1, 2);
    /// 32 is an exact 8x, where symmetry cancels the stripes exactly; 35 is not.
    const auto outSide = GENERATE(std::uint32_t{32}, std::uint32_t{35});
    const auto stripe = [direction](std::uint32_t x, std::uint32_t y) {
        const std::uint32_t phase = direction == 0 ? x : direction == 1 ? y : x + y;
        const float v = phase % 2 == 0 ? 1.0F : 0.0F;
        return std::array<float, 4>{v, v, v, 1.0F};
    };
    const auto source = made({256, 256}, stripe);
    const auto result = resample(made({256, 256}, stripe), {outSide, outSide}, filter);

    const float deviation = interiorDeviation(result, 0.5F, 4);
    INFO("alias deviation " << deviation);
    CHECK(deviation < bound);
}

TEST_CASE("A chirp shrunk 8x has its above-Nyquist energy removed", "[resample][alias]") {
    /// Horizontal frequency rising from 0.1 to 0.5 cycles per source pixel.
    /// Everything above 1/16 cycle per source pixel is beyond the output's
    /// Nyquist limit, so past the first ~13% of the width the result must be
    /// near the mean of 0.5.
    const auto filter = GENERATE(ResizeFilter::Lanczos3, ResizeFilter::Bilinear);
    const auto result = resample(made({512, 64},
                                      [](std::uint32_t x, std::uint32_t) {
                                          const double f = 0.1 + 0.4 * x / 512.0;
                                          const auto v = static_cast<float>(
                                              0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * f * x));
                                          return std::array<float, 4>{v, v, v, 1.0F};
                                      }),
                                 {64, 8}, filter);
    float worst = 0.0F;
    for (std::uint32_t x = 16; x < 56; ++x) {
        worst = std::max(worst, std::abs(at(result, x, 4, 0) - 0.5F));
    }
    INFO("chirp residual " << worst);
    CHECK(worst < 0.1F);
}

TEST_CASE("Lanczos ringing never goes below black, genuine negatives survive",
          "[resample][clamp]") {
    const auto spike = [](ImageSize size) {
        return made(size, [](std::uint32_t x, std::uint32_t y) {
            const float v = (x == 8 && y == 8) ? 1.0F : 0.0F;
            return std::array<float, 4>{v, v, v, 1.0F};
        });
    };
    const auto minimum = [](const ImageBuffer& image) {
        float lowest = 0.0F;
        const auto samples = image.samples<float>();
        for (std::size_t i = 0; i < samples.size(); ++i) {
            lowest = std::min(lowest, samples[i]);
        }
        return lowest;
    };

    SECTION("shrinking and enlarging a bright pixel") {
        REQUIRE(minimum(resample(spike({17, 17}), {5, 5}, ResizeFilter::Lanczos3)) >= 0.0F);
        REQUIRE(minimum(resample(spike({17, 17}), {50, 50}, ResizeFilter::Lanczos3)) >= 0.0F);
        REQUIRE(minimum(resample(spike({17, 17}), {11, 40}, ResizeFilter::Lanczos3)) >= 0.0F);
    }
    SECTION("the spike does still ring before the clamp, so the rule has something to do") {
        /// Without the rule Lanczos has negative lobes: a spike with a negative
        /// input present (which disables the clamp) shows them.
        auto image = spike({17, 17});
        image.samples<float>()[0] = -1.0F;
        REQUIRE(minimum(resample(std::move(image), {50, 50}, ResizeFilter::Lanczos3)) < -0.01F);
    }
    SECTION("negative input values are kept") {
        auto negative = made(
            {8, 8}, [](auto, auto) { return std::array<float, 4>{-0.25F, 0.5F, -0.5F, 1.0F}; });
        const auto result = resample(std::move(negative), {3, 3}, ResizeFilter::Lanczos3);
        for (std::uint32_t y = 0; y < 3; ++y) {
            for (std::uint32_t x = 0; x < 3; ++x) {
                REQUIRE(at(result, x, y, 0) == Catch::Approx(-0.25F).margin(1e-6));
                REQUIRE(at(result, x, y, 2) == Catch::Approx(-0.5F).margin(1e-6));
            }
        }
    }
    SECTION("values above 1 are kept") {
        auto bright =
            made({8, 8}, [](auto, auto) { return std::array<float, 4>{4.0F, 4.0F, 4.0F, 1.0F}; });
        const auto result = resample(std::move(bright), {3, 3}, ResizeFilter::Bilinear);
        REQUIRE(at(result, 1, 1, 0) == Catch::Approx(4.0F).margin(1e-5));
    }
    SECTION("an opaque edge rings above its brighter side") {
        /// Only ringing below zero is removed. An opaque image is not clamped to
        /// the range of its neighbourhood, which would make the filter
        /// non-linear everywhere; that clamp is kept for windows with
        /// transparency.
        auto edge = made({16, 4}, [](std::uint32_t x, auto) {
            const float v = x < 8 ? 0.2F : 0.8F;
            return std::array<float, 4>{v, v, v, 1.0F};
        });
        const auto result = resample(std::move(edge), {48, 4}, ResizeFilter::Lanczos3);
        float highest = 0.0F;
        for (std::uint32_t x = 0; x < 48; ++x) {
            highest = std::max(highest, at(result, x, 1, 0));
        }
        REQUIRE(highest > 0.81F);
    }
}

TEST_CASE("Transparent pixels leave no fringe", "[resample][alpha]") {
    /// Left half opaque red, right half transparent but coloured green: if the
    /// colour were filtered without its alpha, the boundary would turn yellowish.
    const auto filter = GENERATE(ResizeFilter::Lanczos3, ResizeFilter::Bilinear);
    const auto halves = [](std::uint32_t x, std::uint32_t) {
        return x < 8 ? std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F}
                     : std::array<float, 4>{0.0F, 1.0F, 0.0F, 0.0F};
    };

    for (const ImageSize size : {ImageSize{5, 4}, ImageSize{40, 4}}) {
        const auto result = resample(made({16, 4}, halves), size, filter);
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const float alpha = at(result, x, 1, 3);
            if (alpha > 0.0F) {
                REQUIRE(at(result, x, 1, 0) == Catch::Approx(1.0F).margin(1e-5));
                REQUIRE(at(result, x, 1, 1) == Catch::Approx(0.0F).margin(1e-5));
            } else {
                /// Fully transparent results are transparent black.
                REQUIRE(at(result, x, 1, 0) == 0.0F);
                REQUIRE(at(result, x, 1, 1) == 0.0F);
            }
        }
        REQUIRE(at(result, 0, 1, 3) > 0.9F);
        REQUIRE(at(result, size.width - 1, 1, 3) < 0.5F);
    }
}

TEST_CASE("A result of the same size is the same buffer", "[resample][identity]") {
    auto source = made({7, 5}, [](std::uint32_t x, std::uint32_t y) {
        return std::array<float, 4>{static_cast<float>(x), static_cast<float>(y), 0.0F, 1.0F};
    });
    const float* data = source.samples<float>().data();

    for (const auto filter : filters) {
        auto result = resample(std::move(source), {7, 5}, filter);
        REQUIRE(result.samples<float>().data() == data);
        source = std::move(result);
    }
}

TEST_CASE("Resampling refuses what it cannot filter", "[resample]") {
    REQUIRE_THROWS_AS(resample(made({4, 4}, [](auto, auto) { return std::array<float, 4>{}; }),
                               {0, 2}, ResizeFilter::Lanczos3),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(resample(ImageBuffer({4, 4}, PixelFormat::RgbU8, workingEncoding), {2, 2},
                               ResizeFilter::Lanczos3),
                      std::invalid_argument);
}

TEST_CASE("A gradient keeps its order and its ends when enlarged", "[resample]") {
    const auto filter = GENERATE(ResizeFilter::Lanczos3, ResizeFilter::Bilinear);
    const auto result = resample(made({4, 1},
                                      [](std::uint32_t x, std::uint32_t) {
                                          const float v = x / 3.0F;
                                          return std::array<float, 4>{v, v, v, 1.0F};
                                      }),
                                 {16, 3}, filter);
    for (std::uint32_t x = 1; x < 16; ++x) {
        REQUIRE(at(result, x, 1, 0) >= at(result, x - 1, 1, 0) - 1e-6F);
    }
    REQUIRE(at(result, 0, 1, 0) >= 0.0F);
    REQUIRE(at(result, 15, 1, 0) <= 1.1F);
}

TEST_CASE("Two colours beside a transparent area stay bounded and unfringed", "[resample][alpha]") {
    const auto filter = GENERATE(ResizeFilter::Lanczos3, ResizeFilter::Bilinear);
    const auto sizeAcross = GENERATE(std::uint32_t{3}, std::uint32_t{5}, std::uint32_t{7});
    const auto source = [] {
        return made({12, 4}, [](std::uint32_t x, std::uint32_t) {
            if (x < 4) {
                return std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F};
            }
            if (x < 8) {
                return std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F};
            }
            return std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F};
        });
    };
    const auto result = resample(source(), {sizeAcross, 4}, filter);
    // Lanczos may overshoot an opaque edge by its own ringing (about a tenth);
    // what must not happen is the colour/alpha quotient blowing up near the
    // transparency, which reached hundreds before the clamp.
    const float ceiling = filter == ResizeFilter::Lanczos3 ? 1.15F : 1.0F;
    for (std::uint32_t x = 0; x < sizeAcross; ++x) {
        for (std::size_t c = 0; c < 4; ++c) {
            const float v = at(result, x, 1, c);
            INFO("x " << x << " channel " << c << " value " << v);
            REQUIRE(std::isfinite(v));
            REQUIRE(v >= 0.0F);
            REQUIRE(v <= (c == 3 ? 1.0F : ceiling));
        }
    }
}

namespace {

/// @brief Whether two buffers hold the same bits, sample for sample.
bool sameBits(const ImageBuffer& first, const ImageBuffer& second) {
    return first.size() == second.size() && std::ranges::equal(first.bytes(), second.bytes());
}

} // namespace

TEST_CASE("The opaque path is bit-identical to the general one on opaque pixels",
          "[resample][opaque]") {
    // Hard edges, noise, values above one and negative channels, so that the
    // rule against ringing is exercised in both directions.
    const auto wide = made({97, 61}, [](std::uint32_t x, std::uint32_t y) {
        const float noise = static_cast<float>((x * 2654435761U + y * 40503U) % 1000) / 1000.0F;
        const bool edge = (x / 5 + y / 7) % 3 == 0;
        return std::array<float, 4>{edge ? 3.5F : noise, (x / 9 + y / 9) % 2 == 0 ? -0.4F : 0.6F,
                                    0.2F + 0.1F * noise, 1.0F};
    });
    const auto plain = made({64, 48}, [](std::uint32_t x, std::uint32_t y) {
        return std::array<float, 4>{(x % 8 < 4) ? 1.0F : 0.0F, static_cast<float>(y) / 48.0F, 0.25F,
                                    1.0F};
    });
    for (const ImageBuffer* source : {&wide, &plain}) {
        for (const ResizeFilter filter : filters) {
            for (const ImageSize size : {ImageSize{13, 9}, ImageSize{50, 11}, ImageSize{7, 40},
                                         ImageSize{1, 1}, ImageSize{200, 130}, ImageSize{97, 5}}) {
                DYNAMIC_SECTION((filter == ResizeFilter::Lanczos3 ? "Lanczos3 " : "Bilinear ")
                                << size.width << "x" << size.height) {
                    const ImageBuffer general = resample(source->clone(), size, filter, false);
                    const ImageBuffer opaque = resample(source->clone(), size, filter, true);
                    REQUIRE(sameBits(general, opaque));
                    for (std::size_t i = 3; i < opaque.samples<float>().size(); i += 4) {
                        REQUIRE(opaque.samples<float>()[i] == 1.0F);
                    }
                }
            }
        }
    }
}

TEST_CASE("Developing an opaque photograph gives exactly opaque pixels, resized or not",
          "[resample][opaque]") {
    // Rotation and a crop blend neighbours, and the resize filters after them:
    // alpha must stay exactly one through both, which is what the fast path
    // and the JPEG export rely on.
    const auto source = made({90, 70}, [](std::uint32_t x, std::uint32_t y) {
        return std::array<float, 4>{static_cast<float>(x) / 90.0F, static_cast<float>(y) / 70.0F,
                                    0.5F, 1.0F};
    });
    DevelopSettings settings;
    settings.geometry.straighten = 11.0;
    for (const RenderRequest& request :
         {RenderRequest{}, RenderRequest{.size = RenderRequest::Scale{0.37}},
          RenderRequest{.size = RenderRequest::FitInside{40, 40}, .filter = ResizeFilter::Bilinear},
          RenderRequest{.size = RenderRequest::Scale{2.0}, .upscale = Upscale::Allowed}}) {
        const ImageBuffer result = develop(source, DevelopState{settings}, request);
        for (std::size_t i = 3; i < result.samples<float>().size(); i += 4) {
            REQUIRE(result.samples<float>()[i] == 1.0F);
        }
    }
    REQUIRE(planFor(source, DevelopState{settings}, {.size = RenderRequest::Scale{0.37}})
                .resize->opaque);
}

TEST_CASE("Developing is the same through the opaque path as through the general one",
          "[resample][opaque]") {
    const auto source = made({70, 50}, [](std::uint32_t x, std::uint32_t y) {
        return std::array<float, 4>{(x / 6 + y / 6) % 2 == 0 ? 0.9F : 0.05F,
                                    static_cast<float>(x) / 70.0F, 0.3F, 1.0F};
    });
    DevelopSettings settings;
    settings.geometry.straighten = -6.0;
    const RenderRequest request{.size = RenderRequest::Scale{0.43}};
    const ProcessingPlan plan = planFor(source, DevelopState{settings}, request);
    REQUIRE(plan.resize->opaque);

    // Without a request develop() stops at the cropped pixels; resampling them
    // by hand with no claim of opacity is the general path. This shows the two
    // paths agree, not that develop() took the fast path: they are bit-identical,
    // so nothing here could tell. The plan's opaque flag is checked above.
    const ImageBuffer cropped = develop(source, DevelopState{settings});
    REQUIRE(
        sameBits(develop(source, DevelopState{settings}, request),
                 resample(cropped.clone(), plan.resize->outputSize, plan.resize->filter, false)));
}
