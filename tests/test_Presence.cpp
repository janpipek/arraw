#include "ColorSpaces.h"
#include "Presence.h"
#include "ProcessingPlan.h"
#include "RowBands.h"
#include "support/Fixtures.h"
#include "support/RowBandLimit.h"

#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <ImageBuffer.h>
#include <ImageImport.h>
#include <ImagePyramid.h>
#include <Photo.h>
#include <PresenceSettings.h>
#include <RenderCheckpoint.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

using namespace arraw;

/// Texture, Clarity and Dehaze: the Presence context and its controls (ADR 041).

namespace {

/// @brief A working-space image whose grey luminance each pixel gets from a function.
ImageBuffer greyOf(ImageSize size, const std::function<float(std::uint32_t, std::uint32_t)>& at) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    const auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const float value = at(x, y);
            float* pixel = &samples[(static_cast<std::size_t>(y) * size.width + x) * 4];
            pixel[0] = value;
            pixel[1] = value;
            pixel[2] = value;
            pixel[3] = 1.0F;
        }
    }
    return image;
}

/// @brief A coloured image: a grey function tinted warm, so hue and luminance both matter.
ImageBuffer tintedOf(ImageSize size, const std::function<float(std::uint32_t, std::uint32_t)>& at) {
    ImageBuffer image = greyOf(size, at);
    const auto samples = image.samples<float>();
    for (std::size_t index = 0; index < samples.size(); index += 4) {
        samples[index] *= 1.2F;
        samples[index + 2] *= 0.7F;
    }
    return image;
}

/// @brief Settings with only the Presence controls set, and no highlight roll-off.
DevelopSettings presence(float texture, float clarity, float dehaze) {
    DevelopSettings settings;
    settings.tone.filmicHighlights = 0.0F;
    settings.presence = {.texture = texture, .clarity = clarity, .dehaze = dehaze};
    return settings;
}

/// @brief Working luminance of a pixel of a developed image.
float luminanceAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto samples = image.samples<float>();
    const float* pixel = &samples[(static_cast<std::size_t>(y) * image.size().width + x) * 4];
    return colorspaces::workingLuminance[0] * pixel[0] +
           colorspaces::workingLuminance[1] * pixel[1] +
           colorspaces::workingLuminance[2] * pixel[2];
}

/// @brief Standard deviation of log2 luminance over a whole image.
double logSpread(const ImageBuffer& image) {
    double sum = 0.0;
    double squares = 0.0;
    const ImageSize size = image.size();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double value = std::log2(luminanceAt(image, x, y));
            sum += value;
            squares += value * value;
        }
    }
    const auto count = static_cast<double>(size.pixelCount());
    const double mean = sum / count;
    return std::sqrt(std::max(squares / count - mean * mean, 0.0));
}

/// @brief Whether two buffers hold the same bytes.
bool sameBytes(const ImageBuffer& first, const ImageBuffer& second) {
    if (first.size() != second.size() || first.format() != second.format()) {
        return false;
    }
    return std::ranges::equal(first.samples<float>(), second.samples<float>());
}

/// @brief Mean absolute difference of the colour channels of two images of one size.
double meanDifference(const ImageBuffer& first, const ImageBuffer& second) {
    const auto a = first.samples<float>();
    const auto b = second.samples<float>();
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t index = 0; index < a.size(); index += 4) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            sum += std::abs(static_cast<double>(a[index + channel]) - b[index + channel]);
            ++count;
        }
    }
    return sum / static_cast<double>(count);
}

/// @brief Per-sample difference of two images of one size, as an image.
ImageBuffer differenceOf(const ImageBuffer& first, const ImageBuffer& second) {
    ImageBuffer result = first.clone();
    const auto out = result.samples<float>();
    const auto b = second.samples<float>();
    for (std::size_t index = 0; index < out.size(); index += 4) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            out[index + channel] -= b[index + channel];
        }
    }
    return result;
}

/// @brief A scene of smooth shapes and some fine detail, for the level and region tests.
float sceneAt(std::uint32_t x, std::uint32_t y, double scale) {
    const double u = x * scale;
    const double v = y * scale;
    const double shapes =
        0.5 * std::sin(u / 37.0) * std::cos(v / 23.0) + 0.3 * std::sin((u + v) / 61.0);
    const double detail = 0.05 * std::sin(u / 2.3) * std::sin(v / 3.1);
    return static_cast<float>(0.18 * std::exp2(shapes + detail));
}

} // namespace

TEST_CASE("With every control at zero Presence does not exist", "[presence]") {
    const ImageBuffer source =
        tintedOf({64, 48}, [](auto x, auto y) { return sceneAt(x, y, 1.0); });
    const ProcessingPlan plan = planFor(source, DevelopState{presence(0.0F, 0.0F, 0.0F)});
    REQUIRE(plan.presence == PresencePlan{});
    REQUIRE_FALSE(plan.presence.active());
    REQUIRE(plan == planFor(source, DevelopState{presence(0.0F, 0.0F, 0.0F)}));
    REQUIRE(hasPresence(PresenceSettings{}) == false);
    REQUIRE(DevelopSettings{}.presence == PresenceSettings{});

    // The chain returns the colour itself, not a colour scaled by one.
    const Colour colour{0.3F, 0.2F, 0.1F};
    REQUIRE(applyPresence(plan.presence, colour, -1.0F, {}) == colour);
    REQUIRE(developPixel(plan, colour, {.fineBase = 7.0F, .coarseBase = -3.0F}) ==
            developPixel(plan, colour));

    SECTION("each control alone switches on only the base it reads") {
        const PresencePlan texture =
            planFor(source, DevelopState{presence(30.0F, 0.0F, 0.0F)}).presence;
        REQUIRE(texture.fine.active());
        REQUIRE_FALSE(texture.coarse.active());
        const PresencePlan clarity =
            planFor(source, DevelopState{presence(0.0F, 30.0F, 0.0F)}).presence;
        REQUIRE_FALSE(clarity.fine.active());
        REQUIRE(clarity.coarse.active());
        REQUIRE_FALSE(clarity.haze.active());
        const PresencePlan hazier =
            planFor(source, DevelopState{presence(0.0F, 0.0F, -30.0F)}).presence;
        REQUIRE_FALSE(hazier.coarse.active());
        REQUIRE(hazier.haze.active());
        REQUIRE(hazier.dehaze == -0.3F);
        // A negative Dehaze measures the mean, a positive one the floor.
        REQUIRE(hazier.haze.window == 0);
        const PresencePlan clearer =
            planFor(source, DevelopState{presence(0.0F, 0.0F, 30.0F)}).presence;
        REQUIRE(clearer.haze.window > 0);
        // And over a narrower blur: the floor keeps to its edges, the mean is broad.
        REQUIRE(clearer.haze.sigma < hazier.haze.sigma);
        REQUIRE(clearer.coarseReduction() == hazier.coarseReduction());
    }
}

TEST_CASE("Presence resolves its radii from the sensor and the long edge", "[presence][plan]") {
    const ImageBuffer full = greyOf({6000, 4}, [](auto, auto) { return 0.18F; });
    const PresencePlan plan = planFor(full, DevelopState{presence(50.0F, 50.0F, 50.0F)}).presence;
    // Texture: 4 sensor pixels on a grid of 2.
    REQUIRE(plan.fine.reduction == 2);
    REQUIRE(plan.fine.sigma == 2.0F);
    REQUIRE(plan.fine.radius == 6);
    // Clarity: 1% of 6000 is 60 sensor pixels; the cell is the largest power of
    // two leaving four cells a sigma.
    REQUIRE(plan.coarse.reduction == 8);
    REQUIRE(plan.coarse.sigma == 7.5F);
    REQUIRE(plan.coarse.radius == 23);
    REQUIRE(plan.coarse.window == 0);
    // Dehaze: the same cells; a floor opened by 3% of 6000, 180 sensor
    // pixels, and smoothed by a quarter of a percent, 15.
    REQUIRE(plan.haze.reduction == 8);
    REQUIRE(plan.haze.window == 23);
    REQUIRE(plan.haze.sigma == 1.875F);
    REQUIRE(plan.haze.radius == 6);
    // The opening reconstructed by as many steps as the window's radius.
    REQUIRE(plan.haze.reconstruction == 23);
    REQUIRE(plan.coarse.reconstruction == 0);
    REQUIRE(plan.lumaRow == colorspaces::workingLuminance);
    // The margin a region's footprint would need: Dehaze's opening (a window
    // for its minimum and one for its maximum), its reconstruction and its
    // blur, in source pixels.
    REQUIRE(presenceReach(plan) == (2 * 23 + 23 + 6 + 2) * 8);
    REQUIRE(presenceReach(PresencePlan{}) == 0);

    SECTION("a pyramid level covers the same sensor pixels per cell") {
        ImageBuffer level = greyOf({1500, 1}, [](auto, auto) { return 0.18F; });
        level.setPixelScale(4.0);
        const PresencePlan reduced =
            planFor(level, DevelopState{presence(50.0F, 50.0F, 50.0F)}).presence;
        REQUIRE(reduced.coarse.reduction == 2);
        REQUIRE(reduced.coarse.sigma == plan.coarse.sigma);
        REQUIRE(reduced.coarse.radius == plan.coarse.radius);
        REQUIRE(reduced.haze.reduction == 2);
        REQUIRE(reduced.haze.window == plan.haze.window);
        REQUIRE(reduced.haze.sigma == plan.haze.sigma);
        // Texture's four sensor pixels are one pixel of the level, its finest scale.
        REQUIRE(reduced.fine.reduction == 1);
        REQUIRE(reduced.fine.sigma == 1.0F);

        ImageBuffer deeper = greyOf({750, 1}, [](auto, auto) { return 0.18F; });
        deeper.setPixelScale(8.0);
        const PresencePlan deep =
            planFor(deeper, DevelopState{presence(50.0F, 50.0F, 0.0F)}).presence;
        REQUIRE(deep.coarse.reduction == 1);
        REQUIRE(deep.coarse.sigma == 7.5F);
        // Held at one pixel of the level rather than vanishing.
        REQUIRE(deep.fine.sigma == minimumPresenceSigma);
    }

    SECTION("a photograph resolves as its full-resolution pixels do") {
        const Photo photo = openPhoto(test::fixture("linear-32x24-skewed.dng"));
        const Photo edited = photo.with(DevelopState{presence(20.0F, 40.0F, 10.0F)});
        const ImageBuffer pixels = loadImage(photo.path());
        REQUIRE(planFor(edited).presence == planFor(pixels, edited.state()).presence);
    }
}

TEST_CASE("White balance and exposure never reach the Presence context", "[presence][plan]") {
    const ImageBuffer camera = loadImage(test::fixture("linear-32x24-skewed.dng"));
    DevelopSettings base = presence(40.0F, 60.0F, 30.0F);
    DevelopSettings moved = base;
    moved.tone.exposure = 1.3F;
    moved.tone.contrast = 40.0F;
    moved.color.whiteBalance = WhiteBalanceMode::Custom;
    moved.color.temperature = 3200.0F;
    moved.color.tint = 15.0F;
    const ProcessingPlan first = planFor(camera, DevelopState{base});
    const ProcessingPlan second = planFor(camera, DevelopState{moved});
    REQUIRE_FALSE(first.toWorking == second.toWorking);
    REQUIRE(first.presence == second.presence);
    REQUIRE(presenceContextFieldsOf(first.presence) == presenceContextFieldsOf(second.presence));

    // The amounts do not reach the context either; only which bases exist does.
    DevelopSettings stronger = base;
    stronger.presence = {.texture = -80.0F, .clarity = 10.0F, .dehaze = 90.0F};
    REQUIRE(presenceContextFieldsOf(planFor(camera, DevelopState{stronger}).presence) ==
            presenceContextFieldsOf(first.presence));

    // The context is a function of the pixels and those fields alone.
    const PresenceContext context = presenceContextOf(camera, first.presence);
    const PresenceContext again = presenceContextOf(camera, second.presence);
    REQUIRE(context.fine.cells == again.fine.cells);
    REQUIRE(context.coarse.cells == again.coarse.cells);
    REQUIRE(context.coarseCells.cells == again.coarseCells.cells);
    REQUIRE(context.haze.cells == again.haze.cells);

    // An exposure change scales the toned result but leaves the detail alone:
    // in neutral tone, the developed image is the same image times the gain.
    DevelopSettings plain = presence(50.0F, 70.0F, 0.0F);
    DevelopSettings brighter = plain;
    brighter.tone.exposure = 1.0F;
    const ImageBuffer grey = greyOf({48, 32}, [](auto x, auto y) { return sceneAt(x, y, 3.0); });
    const ImageBuffer dim = develop(grey, DevelopState{plain});
    const ImageBuffer bright = develop(grey, DevelopState{brighter});
    // Clarity's midtone weight reads the toned luminance, so only Texture's
    // contribution is exactly a gain; with Texture alone the two match.
    plain.presence.clarity = 0.0F;
    brighter.presence.clarity = 0.0F;
    const ImageBuffer dimTexture = develop(grey, DevelopState{plain});
    const ImageBuffer brightTexture = develop(grey, DevelopState{brighter});
    const auto d = dimTexture.samples<float>();
    const auto b = brightTexture.samples<float>();
    for (std::size_t index = 0; index < d.size(); index += 4) {
        REQUIRE(std::abs(b[index] - 2.0F * d[index]) <= 1e-5F * b[index]);
    }
    REQUIRE_FALSE(sameBytes(dim, bright));
}

TEST_CASE("Clarity raises local contrast at a step, with a bounded halo", "[presence]") {
    // Two stops, from 0.05 to 0.2, at x = 200 of 400: Clarity's sigma is 1% of
    // the long edge, 4 pixels.
    const ImageSize size{400, 40};
    const ImageBuffer step = greyOf(size, [](auto x, auto) { return x < 200 ? 0.05F : 0.2F; });
    const ImageBuffer plain = develop(step, DevelopState{presence(0.0F, 0.0F, 0.0F)});
    const PresencePlan plan = planFor(step, DevelopState{presence(0.0F, 100.0F, 0.0F)}).presence;
    REQUIRE(plan.coarse.reduction == 1);
    REQUIRE(plan.coarse.sigma == 4.0F);

    for (const float clarity : {100.0F, 50.0F, -100.0F}) {
        INFO("clarity " << clarity);
        const ImageBuffer clear = develop(step, DevelopState{presence(0.0F, clarity, 0.0F)});
        const auto stopsAt = [&](std::uint32_t x) {
            return std::log2(luminanceAt(clear, x, 20) / luminanceAt(plain, x, 20));
        };
        double worst = 0.0;
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double moved = stopsAt(x);
            worst = std::max(worst, std::abs(moved));
            // The bound: no pixel moves by more than |Clarity| / 100 stops.
            REQUIRE(std::abs(moved) <= std::abs(clarity) / 100.0 * clarityLimitStops + 1e-5);
            // Five sigmas and the bilinear read's cell away, nothing moves.
            if (x + 22 < 200 || x >= 222) {
                REQUIRE(std::abs(moved) < 2e-3);
            }
        }
        const double sign = clarity > 0.0F ? 1.0 : -1.0;
        // At the edge the dark side darkens and the light side lightens (or the reverse).
        REQUIRE(sign * stopsAt(199) < -0.1 * std::abs(clarity) / 100.0);
        REQUIRE(sign * stopsAt(200) > 0.1 * std::abs(clarity) / 100.0);
        if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
            std::fprintf(stderr, "clarity %g: worst %.3f stops, edge %.3f / %.3f\n", clarity, worst,
                         stopsAt(199), stopsAt(200));
        }
    }

    SECTION("a hard edge of eight stops still moves no pixel by more than the bound") {
        const ImageBuffer hard =
            greyOf(size, [](auto x, auto) { return x < 200 ? 0.003F : 0.75F; });
        const ImageBuffer before = develop(hard, DevelopState{presence(0.0F, 0.0F, 0.0F)});
        const ImageBuffer after = develop(hard, DevelopState{presence(0.0F, 100.0F, 0.0F)});
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const double moved = std::log2(luminanceAt(after, x, 5) / luminanceAt(before, x, 5));
            REQUIRE(std::abs(moved) <= clarityLimitStops + 1e-5);
        }
    }
}

TEST_CASE("Texture moves fine detail and leaves broad shapes alone", "[presence]") {
    // A tenth of a stop of modulation, at a period of 4 pixels and of 256.
    const ImageSize size{512, 16};
    const auto wave = [](double period) {
        return [period](std::uint32_t x, std::uint32_t) {
            return static_cast<float>(
                0.18 * std::exp2(0.1 * std::sin(2.0 * std::numbers::pi * (x + 0.5) / period)));
        };
    };
    const ImageBuffer fine = greyOf(size, wave(4.0));
    const ImageBuffer broad = greyOf(size, wave(256.0));
    const auto gain = [](const ImageBuffer& image, float texture) {
        return logSpread(develop(image, DevelopState{presence(texture, 0.0F, 0.0F)})) /
               logSpread(develop(image, DevelopState{presence(0.0F, 0.0F, 0.0F)}));
    };
    const double fineUp = gain(fine, 100.0F);
    const double broadUp = gain(broad, 100.0F);
    const double fineDown = gain(fine, -100.0F);
    const double broadDown = gain(broad, -100.0F);
    CAPTURE(fineUp, broadUp, fineDown, broadDown);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "texture: fine x%.3f / x%.3f, broad x%.3f / x%.3f\n", fineUp, fineDown,
                     broadUp, broadDown);
    }
    REQUIRE(fineUp > 1.5);
    REQUIRE(std::abs(broadUp - 1.0) < 0.05);
    // Negative Texture smooths the fine detail, again sparing the broad.
    REQUIRE(fineDown < 0.4);
    REQUIRE(std::abs(broadDown - 1.0) < 0.05);
}

TEST_CASE("Negative Clarity softens mid-scale contrast", "[presence]") {
    const ImageSize size{320, 240};
    const ImageBuffer scene = tintedOf(size, [](auto x, auto y) {
        return static_cast<float>(0.18 * std::exp2(0.6 * std::sin(x / 2.0) * std::cos(y / 3.0)));
    });
    const double plain = logSpread(develop(scene, DevelopState{presence(0.0F, 0.0F, 0.0F)}));
    const double soft = logSpread(develop(scene, DevelopState{presence(0.0F, -100.0F, 0.0F)}));
    const double crisp = logSpread(develop(scene, DevelopState{presence(0.0F, 100.0F, 0.0F)}));
    CAPTURE(plain, soft, crisp);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "clarity spread: x%.3f at -100, x%.3f at 100\n", soft / plain,
                     crisp / plain);
    }
    REQUIRE(soft < 0.8 * plain);
    REQUIRE(crisp > 1.2 * plain);
}

TEST_CASE("Dehaze raises the contrast of a veiled image", "[presence]") {
    // A scene of tones from 0.02 to 0.3, under a neutral veil, in fine detail:
    // a pattern of about 12 pixels. Every window of Dehaze's floor holds its
    // darkest tone, so the veil is found and taken off at this scale too.
    const ImageSize size{600, 400};
    const auto scene = [](std::uint32_t x, std::uint32_t y) {
        const double shape = 0.5 + 0.5 * std::sin(x / 2.0) * std::sin(y / 3.0);
        return static_cast<float>(0.02 + 0.28 * shape);
    };
    const ImageBuffer veiled =
        tintedOf(size, [&](auto x, auto y) { return 0.6F * scene(x, y) + 0.3F; });
    const ImageBuffer plain = develop(veiled, DevelopState{presence(0.0F, 0.0F, 0.0F)});
    const ImageBuffer clear = develop(veiled, DevelopState{presence(0.0F, 0.0F, 100.0F)});
    const ImageBuffer hazier = develop(veiled, DevelopState{presence(0.0F, 0.0F, -100.0F)});
    const double before = logSpread(plain);
    const double after = logSpread(clear);
    const double added = logSpread(hazier);
    CAPTURE(before, after, added);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "dehaze spread: x%.3f at 100, x%.3f at -100\n", after / before,
                     added / before);
    }
    REQUIRE(after > 1.3 * before);
    REQUIRE(added < 0.8 * before);

    // The veil comes off: the darkest tone gets darker, and nothing crosses black.
    float darkestBefore = std::numeric_limits<float>::max();
    float darkestAfter = std::numeric_limits<float>::max();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            darkestBefore = std::min(darkestBefore, luminanceAt(plain, x, y));
            darkestAfter = std::min(darkestAfter, luminanceAt(clear, x, y));
        }
    }
    REQUIRE(darkestAfter < 0.85F * darkestBefore);
    REQUIRE(darkestAfter > 0.0F);

    // And colour comes back with it, restrained.
    const auto chromaAt = [](const ImageBuffer& image) {
        const auto samples = image.samples<float>();
        double sum = 0.0;
        for (std::size_t index = 0; index < samples.size(); index += 4) {
            sum += (samples[index] - samples[index + 2]) /
                   (samples[index] + samples[index + 1] + samples[index + 2]);
        }
        return sum;
    };
    REQUIRE(chromaAt(clear) > chromaAt(plain));
    REQUIRE(chromaAt(hazier) < chromaAt(plain));
}

TEST_CASE("Dehaze takes off a broad veil", "[presence]") {
    // Blocks of 40 pixels, 0.02 and 0.3, tinted: shapes above Clarity's sigma
    // (12 pixels here) and within Dehaze's window (36). The lower half sits
    // under a uniform veil of 0.3, as a distant part of a landscape would.
    const ImageSize size{1200, 800};
    const auto block = [](std::uint32_t x, std::uint32_t y) {
        return ((x / 40) + (y / 40)) % 2 == 0;
    };
    const ImageBuffer veiled = tintedOf(size, [&](auto x, auto y) {
        const float scene = block(x, y) ? 0.3F : 0.02F;
        return y >= 400 ? 0.6F * scene + 0.3F : scene;
    });
    const ImageBuffer plain = develop(veiled, DevelopState{presence(0.0F, 0.0F, 0.0F)});
    const ImageBuffer clear = develop(veiled, DevelopState{presence(0.0F, 0.0F, 100.0F)});
    const ImageBuffer hazier = develop(veiled, DevelopState{presence(0.0F, 0.0F, -100.0F)});

    // Mean luminance of the centres of the light or the dark blocks in a band of rows.
    const auto blocksIn = [&](const ImageBuffer& image, std::uint32_t top, std::uint32_t bottom,
                              bool light) {
        double sum = 0.0;
        std::size_t count = 0;
        for (std::uint32_t y = top; y < bottom; ++y) {
            for (std::uint32_t x = 0; x < size.width; ++x) {
                if (x % 40 >= 15 && x % 40 < 25 && y % 40 >= 15 && y % 40 < 25 &&
                    block(x, y) == light) {
                    sum += luminanceAt(image, x, y);
                    ++count;
                }
            }
        }
        return sum / static_cast<double>(count);
    };
    // Contrast between blocks, a scale above Clarity's, well inside the veil
    // (four of Dehaze's sigmas from its edge), and the clear half against the
    // veiled half.
    const auto contrast = [&](const ImageBuffer& image) {
        return blocksIn(image, 520, 800, true) / blocksIn(image, 520, 800, false);
    };
    const auto halves = [&](const ImageBuffer& image) {
        return blocksIn(image, 0, 280, true) / blocksIn(image, 520, 800, true);
    };
    const double before = contrast(plain);
    const double after = contrast(clear);
    const double added = contrast(hazier);
    const double halvesBefore = halves(plain);
    const double halvesAfter = halves(clear);
    CAPTURE(before, after, added, halvesBefore, halvesAfter);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr,
                     "broad veil: block contrast %.3f -> %.3f at 100, %.3f at -100; "
                     "clear / veiled %.3f -> %.3f\n",
                     before, after, added, halvesBefore, halvesAfter);
    }
    // 1.54 under the veil, 15 without it: Dehaze 100 takes off most of the veil.
    REQUIRE(after > 1.4 * before);
    REQUIRE(added < 0.9 * before);
    // The veiled half darkens more than the clear one.
    REQUIRE(halvesAfter > 1.3 * halvesBefore);

    // Nothing crosses black: every channel of every pixel stays above zero.
    for (const float value : clear.samples<float>()) {
        REQUIRE(value > 0.0F);
    }
}

TEST_CASE("Dehaze is the same at every exposure", "[presence]") {
    // In neutral tone a brighter exposure is the same image times its gain, and
    // Dehaze, measured on the as-shot luminance, keeps it so, both ways: the
    // floor's share of a pixel, and the surroundings' mean, are ratios.
    const ImageSize size{300, 200};
    const ImageBuffer veiled = tintedOf(size, [](auto x, auto y) {
        const double shape = 0.5 + 0.5 * std::sin(x / 7.0) * std::sin(y / 5.0);
        return static_cast<float>(0.5 * (0.6 * (0.02 + 0.28 * shape) + 0.3));
    });
    for (const float dehaze : {100.0F, -100.0F}) {
        INFO("dehaze " << dehaze);
        DevelopSettings dim = presence(0.0F, 0.0F, dehaze);
        DevelopSettings bright = dim;
        bright.tone.exposure = 1.0F;
        const ImageBuffer d = develop(veiled, DevelopState{dim});
        const ImageBuffer b = develop(veiled, DevelopState{bright});
        REQUIRE_FALSE(sameBytes(d, develop(veiled, DevelopState{presence(0.0F, 0.0F, 0.0F)})));
        const auto low = d.samples<float>();
        const auto high = b.samples<float>();
        for (std::size_t index = 0; index < low.size(); index += 4) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                REQUIRE(std::abs(high[index + channel] - 2.0F * low[index + channel]) <=
                        2e-5F * high[index + channel]);
            }
        }
    }
}

TEST_CASE("Dehaze takes as much off a bright area at its edge as inside it", "[presence]") {
    // A step from 0.03 to 0.3, both sides wider than Dehaze's window (36
    // pixels), the edge off the cells' grid of 2 pixels. Every window of the
    // bright side up to the edge holds only itself and the edge, so its floor
    // is its own: Dehaze 100 must take the same off right up to the edge, not
    // leave a lighter rim where the dark side's floor would spread in.
    const ImageSize size{1200, 48};
    const std::uint32_t edge = 601;
    const ImageBuffer step = greyOf(size, [&](auto x, auto) { return x < edge ? 0.03F : 0.3F; });
    REQUIRE(planFor(step, DevelopState{presence(0.0F, 0.0F, 100.0F)}).presence.haze.reduction == 2);
    const ImageBuffer plain = develop(step, DevelopState{presence(0.0F, 0.0F, 0.0F)});
    const ImageBuffer clear = develop(step, DevelopState{presence(0.0F, 0.0F, 100.0F)});
    const auto stopsAt = [&](std::uint32_t x) {
        return std::log2(luminanceAt(clear, x, size.height / 2) /
                         luminanceAt(plain, x, size.height / 2));
    };
    const float interior = stopsAt(size.width - 100);
    float worst = 0.0F;
    std::uint32_t rim = 0;
    for (std::uint32_t x = edge; x < size.width; ++x) {
        const float off = std::abs(stopsAt(x) - interior);
        worst = std::max(worst, off);
        if (off > 0.05F) {
            rim = x - edge + 1;
        }
    }
    const float darkSide = stopsAt(edge - 1);
    CAPTURE(interior, worst, rim, darkSide);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr,
                     "dehaze edge: interior %.3f stops, worst %.3f stops off it, rim %u px, "
                     "dark side %.3f stops\n",
                     interior, worst, rim, darkSide);
    }
    REQUIRE(interior < -1.0F);
    REQUIRE(worst < 0.01F);
    // The dark side, at its own floor, keeps 40% of itself up to the edge too.
    REQUIRE(std::abs(darkSide - interior) < 0.01F);
}

TEST_CASE("Dehaze takes as much off a round bright area at its edge as inside it", "[presence]") {
    // Disks of 150, 300 and 600 pixels, 0.3 on 0.03 under a uniform veil, in a
    // frame of 2400: cells of 4 and a window of 18 cells (72 pixels). The square
    // window does not reach the edge of a disk on its axis-facing sides; the
    // opening's reconstruction brings the floor back up to it, so Dehaze 100
    // takes as much off there as at the centre, and as on the diagonals.
    const ImageSize size{2400, 1400};
    struct Disk {
        double x;
        double y;
        double radius;
    };
    const std::array disks{Disk{650.3, 700.7, 600.0}, Disk{1700.3, 400.7, 300.0},
                           Disk{1700.3, 1100.7, 150.0}};
    const ImageBuffer scene = greyOf(size, [&](auto x, auto y) {
        const bool inside = std::ranges::any_of(disks, [&](const Disk& disk) {
            return std::hypot(x - disk.x, y - disk.y) < disk.radius;
        });
        return 0.6F * (inside ? 0.3F : 0.03F) + 0.2F;
    });
    const PresencePlan plan = planFor(scene, DevelopState{presence(0.0F, 0.0F, 100.0F)}).presence;
    REQUIRE(plan.haze.reduction == 4);
    REQUIRE(plan.haze.window == 18);
    const ImageBuffer plain = develop(scene, DevelopState{presence(0.0F, 0.0F, 0.0F)});
    const ImageBuffer clear = develop(scene, DevelopState{presence(0.0F, 0.0F, 100.0F)});
    const auto stopsAt = [&](std::uint32_t x, std::uint32_t y) {
        return std::log2(luminanceAt(clear, x, y) / luminanceAt(plain, x, y));
    };
    for (const Disk& disk : disks) {
        const auto centreX = static_cast<std::uint32_t>(disk.x);
        const auto centreY = static_cast<std::uint32_t>(disk.y);
        const float centre = stopsAt(centreX, centreY);
        // Every pixel inside the disk along the rays to its four axis-facing
        // sides and its four diagonals, from the centre to the edge.
        float worstAxis = 0.0F;
        float worstDiagonal = 0.0F;
        for (int ray = 0; ray < 8; ++ray) {
            const double angle = ray * std::numbers::pi / 4.0;
            for (double distance = 0.0;; distance += 1.0) {
                const auto x =
                    static_cast<std::uint32_t>(std::lround(centreX + distance * std::cos(angle)));
                const auto y =
                    static_cast<std::uint32_t>(std::lround(centreY + distance * std::sin(angle)));
                if (std::hypot(x - disk.x, y - disk.y) >= disk.radius) {
                    break;
                }
                float& worst = ray % 2 == 0 ? worstAxis : worstDiagonal;
                worst = std::max(worst, std::abs(stopsAt(x, y) - centre));
            }
        }
        CAPTURE(disk.radius, centre, worstAxis, worstDiagonal);
        if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
            std::fprintf(stderr,
                         "dehaze disk of %.0f px: centre %.3f stops, worst %.4f stops off it on "
                         "the axes, %.4f on the diagonals\n",
                         disk.radius, centre, worstAxis, worstDiagonal);
        }
        REQUIRE(centre < -0.5F);
        REQUIRE(worstAxis < 0.02F);
        REQUIRE(worstDiagonal < 0.02F);
    }
}

TEST_CASE("Dehaze raises fine detail and noise in a flat area no more than Texture", "[presence]") {
    // Mid grey, 3400 pixels (cells of 8): a quarter of a stop of modulation at
    // a period of 5 pixels, finer than a cell, and of noise. Dehaze measures
    // the floor's share against the cell, not the pixel, so what is finer than
    // a cell is scaled smoothly rather than expanded.
    const ImageSize size{3400, 16};
    const auto wave = [](double period) {
        return [period](std::uint32_t x, std::uint32_t) {
            return static_cast<float>(
                0.18 * std::exp2(0.25 * std::sin(2.0 * std::numbers::pi * (x + 0.5) / period)));
        };
    };
    // A fixed hash, so the noise is the same on every standard library.
    const auto noise = [](std::uint32_t x, std::uint32_t y) {
        std::uint32_t h = x * 73856093U ^ y * 19349663U;
        h ^= h >> 13;
        h *= 0x5bd1e995U;
        h ^= h >> 15;
        const double unit = static_cast<double>(h & 0xffffU) / 65535.0;
        return static_cast<float>(0.18 * std::exp2(0.25 * (2.0 * unit - 1.0)));
    };
    const ImageBuffer fine = greyOf(size, wave(5.0));
    const ImageBuffer grain = greyOf(size, noise);
    const ImageBuffer mid = greyOf(size, wave(128.0));
    REQUIRE(planFor(fine, DevelopState{presence(0.0F, 0.0F, 100.0F)}).presence.haze.reduction == 8);
    const auto gain = [](const ImageBuffer& image, float texture, float dehaze) {
        return logSpread(develop(image, DevelopState{presence(texture, 0.0F, dehaze)})) /
               logSpread(develop(image, DevelopState{presence(0.0F, 0.0F, 0.0F)}));
    };
    const double textureFine = gain(fine, 100.0F, 0.0F);
    const double textureNoise = gain(grain, 100.0F, 0.0F);
    const double dehazeFine = gain(fine, 0.0F, 100.0F);
    const double dehazeNoise = gain(grain, 0.0F, 100.0F);
    const double dehazeMid = gain(mid, 0.0F, 100.0F);
    CAPTURE(textureFine, textureNoise, dehazeFine, dehazeNoise, dehazeMid);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr,
                     "dehaze in a flat area: 5 px x%.3f (texture x%.3f), noise x%.3f (texture "
                     "x%.3f), 128 px x%.3f\n",
                     dehazeFine, textureFine, dehazeNoise, textureNoise, dehazeMid);
    }
    REQUIRE(dehazeFine <= textureFine);
    REQUIRE(dehazeNoise <= textureNoise);
    // And barely at all: measured x1.006 and x1.022, where a share against the
    // pixel gave about x1.5.
    REQUIRE(dehazeFine <= 1.2);
    REQUIRE(dehazeNoise <= 1.2);
}

TEST_CASE("An infinite pixel under negative Dehaze is not NaN", "[presence]") {
    const PresencePlan plan =
        presencePlanFor({.dehaze = -100.0F}, workingEncoding, 1.0, ImageSize{64, 64});
    const float infinity = std::numeric_limits<float>::infinity();
    const Colour colour =
        applyPresence(plan, {infinity, infinity, infinity},
                      presenceLogLuminance(plan, {infinity, infinity, infinity}), PixelContext{});
    for (const float channel : colour) {
        REQUIRE_FALSE(std::isnan(channel));
    }
}

TEST_CASE("Clarity leaves fine detail to Texture", "[presence]") {
    // 3400 pixels: Clarity's sigma is 34 pixels on cells of 8, and its band
    // runs from the cells to the blur. A tenth of a stop of modulation at a
    // period of 5 pixels (Texture's scale, and not a divisor of the cell, so
    // some of it survives the cells) and of 128 (Clarity's).
    const ImageSize size{3400, 16};
    REQUIRE(planFor(greyOf(size, [](auto, auto) { return 0.18F; }),
                    DevelopState{presence(0.0F, 50.0F, 0.0F)})
                .presence.coarse.reduction == 8);
    const auto wave = [](double period) {
        return [period](std::uint32_t x, std::uint32_t) {
            return static_cast<float>(
                0.18 * std::exp2(0.1 * std::sin(2.0 * std::numbers::pi * (x + 0.5) / period)));
        };
    };
    const ImageBuffer fine = greyOf(size, wave(5.0));
    const ImageBuffer mid = greyOf(size, wave(128.0));
    const auto gain = [](const ImageBuffer& image, float texture, float clarity) {
        return logSpread(develop(image, DevelopState{presence(texture, clarity, 0.0F)})) /
               logSpread(develop(image, DevelopState{presence(0.0F, 0.0F, 0.0F)}));
    };
    const double textureUp = gain(fine, 100.0F, 0.0F);
    const double clarityUp = gain(fine, 0.0F, 100.0F);
    const double clarityDown = gain(fine, 0.0F, -100.0F);
    const double midUp = gain(mid, 0.0F, 100.0F);
    const double midDown = gain(mid, 0.0F, -100.0F);
    CAPTURE(textureUp, clarityUp, clarityDown, midUp, midDown);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr,
                     "clarity band: fine x%.3f / x%.3f (texture x%.3f), mid x%.3f / x%.3f\n",
                     clarityUp, clarityDown, textureUp, midUp, midDown);
    }
    // Clarity raises fine detail by under a tenth of what Texture does...
    REQUIRE(clarityUp - 1.0 < 0.1 * (textureUp - 1.0));
    // ...and at -100 keeps it: no blur of texture or noise.
    REQUIRE(std::abs(clarityDown - 1.0) < 0.05);
    // While at its own scale it does what it did.
    REQUIRE(midUp > 1.5);
    REQUIRE(midDown < 0.6);
}

TEST_CASE("A non-finite pixel stays where it is", "[presence]") {
    // An infinite and a NaN pixel: the context bounds them, so their
    // neighbours develop to finite colours, near what they would without them.
    const ImageSize size{96, 64};
    ImageBuffer source = tintedOf(size, [](auto x, auto y) { return sceneAt(x, y, 2.0); });
    const ImageBuffer clean = source.clone();
    const auto samples = source.samples<float>();
    const std::size_t infinite = (static_cast<std::size_t>(20) * size.width + 30) * 4;
    const std::size_t missing = (static_cast<std::size_t>(40) * size.width + 60) * 4;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        samples[infinite + channel] = std::numeric_limits<float>::infinity();
        samples[missing + channel] = std::numeric_limits<float>::quiet_NaN();
    }
    for (const float dehaze : {60.0F, -60.0F}) {
        INFO("dehaze " << dehaze);
        const DevelopState state{presence(60.0F, 80.0F, dehaze)};
        const ImageBuffer developed = develop(source, state);
        const ImageBuffer reference = develop(clean, state);
        const auto out = developed.samples<float>();
        const auto ref = reference.samples<float>();
        for (std::size_t index = 0; index < out.size(); index += 4) {
            if (index == infinite || index == missing) {
                continue;
            }
            for (std::size_t channel = 0; channel < 3; ++channel) {
                REQUIRE(std::isfinite(out[index + channel]));
            }
        }
        // Far from both, nothing changed at all.
        REQUIRE(out[(static_cast<std::size_t>(5) * size.width + 90) * 4] ==
                ref[(static_cast<std::size_t>(5) * size.width + 90) * 4]);
    }
}

TEST_CASE("Presence keeps black, and a dark pixel's hue", "[presence]") {
    const ImageBuffer source = greyOf({40, 20}, [](auto x, auto) { return x < 20 ? 0.0F : 0.4F; });
    const ImageBuffer developed = develop(source, DevelopState{presence(100.0F, 100.0F, 100.0F)});
    for (std::uint32_t y = 0; y < 20; ++y) {
        for (std::uint32_t x = 0; x < 20; ++x) {
            REQUIRE(luminanceAt(developed, x, y) == 0.0F);
        }
    }
    // Hue: a coloured pixel is scaled, so its channel ratios hold without Dehaze.
    const ImageBuffer tinted =
        tintedOf({64, 64}, [](auto x, auto y) { return sceneAt(x, y, 2.0); });
    const ImageBuffer plain = develop(tinted, DevelopState{presence(0.0F, 0.0F, 0.0F)});
    const ImageBuffer moved = develop(tinted, DevelopState{presence(80.0F, 80.0F, 0.0F)});
    const auto p = plain.samples<float>();
    const auto m = moved.samples<float>();
    for (std::size_t index = 0; index < p.size(); index += 4) {
        REQUIRE(std::abs(m[index] / m[index + 1] - p[index] / p[index + 1]) < 1e-5F);
        REQUIRE(std::abs(m[index + 2] / m[index + 1] - p[index + 2] / p[index + 1]) < 1e-5F);
    }
}

TEST_CASE("A preview level shows the Clarity and Dehaze the full frame does",
          "[presence][preview]") {
    // A scene whose shapes are a few tens of pixels across at full resolution.
    const ImageSize size{1600, 1200};
    const ImageBuffer full = tintedOf(size, [](auto x, auto y) { return sceneAt(x, y, 0.25); });
    const ImageBuffer level = halved(halved(full));
    REQUIRE(level.pixelScale() == 4.0);
    const DevelopState none{presence(0.0F, 0.0F, 0.0F)};
    const DevelopState on{presence(0.0F, 80.0F, 40.0F)};

    // The effect, at the level's size: on the full frame reduced afterwards,
    // and on the level developed directly.
    const ImageBuffer fullEffect =
        differenceOf(halved(halved(develop(full, on))), halved(halved(develop(full, none))));
    const ImageBuffer levelEffect = differenceOf(develop(level, on), develop(level, none));
    const auto zero = [](const ImageBuffer& image) {
        ImageBuffer blank = image.clone();
        std::ranges::fill(blank.samples<float>(), 0.0F);
        return blank;
    };
    const double magnitude = meanDifference(fullEffect, zero(fullEffect));
    const double error = meanDifference(fullEffect, levelEffect);
    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
        std::fprintf(stderr, "level 2: effect %.4g, difference %.4g (%.1f%%)\n", magnitude, error,
                     100.0 * error / magnitude);
    }
    CAPTURE(magnitude, error);
    REQUIRE(magnitude > 0.002);
    REQUIRE(error < 0.02 * magnitude);
}

TEST_CASE("A region render with Presence is the crop of the whole render", "[presence][region]") {
    const ImageBuffer source =
        tintedOf({160, 120}, [](auto x, auto y) { return sceneAt(x, y, 2.0); });
    DevelopSettings settings = presence(60.0F, 70.0F, 30.0F);
    settings.noiseReduction.color = 40.0F;
    const DevelopState state{settings};
    const ImageBuffer whole = develop(source, state);
    const RenderRequest request{
        .region = RenderRequest::Region{.left = 0.25, .top = 0.5, .right = 0.75, .bottom = 1.0}};
    const ImageBuffer part = develop(source, state, request);
    REQUIRE(part.size() == ImageSize{80, 60});
    for (std::uint32_t y = 0; y < 60; ++y) {
        for (std::uint32_t x = 0; x < 80; ++x) {
            const auto a = part.samples<float>();
            const auto b = whole.samples<float>();
            const std::size_t at = (static_cast<std::size_t>(y) * 80 + x) * 4;
            const std::size_t from = (static_cast<std::size_t>(y + 60) * 160 + x + 40) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                REQUIRE(a[at + channel] == b[from + channel]);
            }
        }
    }
}

TEST_CASE("Presence renders resume from the Denoise and pointwise boundaries",
          "[presence][resume]") {
    const ImageBuffer source =
        tintedOf({96, 64}, [](auto x, auto y) { return sceneAt(x, y, 2.0); });
    DevelopSettings settings = presence(30.0F, 50.0F, 20.0F);
    settings.noiseReduction.color = 50.0F;
    const RenderCheckpoint denoised = developUntil(source, DevelopState{settings}, Stage::Denoise);

    // A Presence edit, an exposure and a white balance edit all resume after
    // noise reduction: the context is recomputed from the checkpoint's pixels.
    for (const auto& edit : std::vector<std::function<void(DevelopSettings&)>>{
             [](DevelopSettings& s) { s.presence.clarity = -40.0F; },
             [](DevelopSettings& s) { s.presence.texture = 0.0F; },
             [](DevelopSettings& s) { s.tone.exposure = 0.7F; },
             [](DevelopSettings& s) { s.presence = {}; }}) {
        DevelopSettings after = settings;
        edit(after);
        const DevelopState state{after};
        const RenderCheckpoint resumed = resumeFrom(denoised, source, state, Stage::Effects);
        REQUIRE(sameBytes(resumed.readBack(), develop(source, state)));
    }

    // A crop resumes after the chain.
    const RenderCheckpoint developed =
        developUntil(source, DevelopState{settings}, Stage::Pointwise);
    DevelopSettings cropped = settings;
    cropped.geometry.crop.rectangle = UprightCropRect{0.1, 0.2, 0.8, 0.9};
    REQUIRE(
        sameBytes(resumeFrom(developed, source, DevelopState{cropped}, Stage::Effects).readBack(),
                  develop(source, DevelopState{cropped})));
    // A Presence edit does not resume from the pointwise boundary.
    DevelopSettings clearer = settings;
    clearer.presence.clarity = 90.0F;
    REQUIRE_THROWS_AS(resumeFrom(developed, source, DevelopState{clearer}, Stage::Effects),
                      std::invalid_argument);
}

TEST_CASE("The curve input includes Texture, Clarity and Dehaze", "[presence][sample]") {
    // Lightroom counts them as Basic, and the curve input tap is after Basic Tone (ADR 035).
    const ImageBuffer source =
        tintedOf({64, 48}, [](auto x, auto y) { return sceneAt(x, y, 2.0); });
    const DevelopState plain{presence(0.0F, 0.0F, 0.0F)};
    const DevelopState clear{presence(0.0F, 60.0F, 0.0F)};
    REQUIRE_FALSE(
        sameBytes(sample(source, plain, Tap::CurveInput), sample(source, clear, Tap::CurveInput)));
    REQUIRE_FALSE(sameAtTap(planFor(source, plain), planFor(source, clear), Tap::CurveInput));
    // And a curve edit after it still leaves the sample alone.
    DevelopSettings curved = clear.settings;
    curved.toneCurve.luma = ToneCurve{{{0.0F, 0.0F}, {0.5F, 0.6F}, {1.0F, 1.0F}}};
    REQUIRE(
        sameAtTap(planFor(source, clear), planFor(source, DevelopState{curved}), Tap::CurveInput));
}

TEST_CASE("The threaded Presence context gives the single-threaded bits", "[presence][threads]") {
    const ImageBuffer source =
        tintedOf({517, 389}, [](auto x, auto y) { return sceneAt(x, y, 1.0); });
    const PresencePlan plan = planFor(source, DevelopState{presence(40.0F, 40.0F, 40.0F)}).presence;
    REQUIRE(plan.haze.window > 0);
    const PresenceContext threaded = presenceContextOf(source, plan);
    const PresenceContext single = [&] {
        const test::ScopedRowBandLimit one(1);
        return presenceContextOf(source, plan);
    }();
    REQUIRE(threaded.fine.cells == single.fine.cells);
    REQUIRE(threaded.coarse.cells == single.coarse.cells);
    REQUIRE(threaded.coarseCells.cells == single.coarseCells.cells);
    REQUIRE(threaded.haze.cells == single.haze.cells);
}

TEST_CASE("The threaded chain gives the single-threaded bits", "[presence][threads]") {
    const ImageBuffer source =
        tintedOf({301, 517}, [](auto x, auto y) { return sceneAt(x, y, 1.0); });
    const DevelopState state{presence(30.0F, 60.0F, 40.0F)};
    const ImageBuffer threaded = develop(source, state);
    const ImageBuffer single = [&] {
        const test::ScopedRowBandLimit one(1);
        return develop(source, state);
    }();
    REQUIRE(sameBytes(threaded, single));
}

TEST_CASE("Presence refuses what it cannot resolve", "[presence]") {
    const ImageBuffer source = greyOf({8, 8}, [](auto, auto) { return 0.2F; });
    for (const float bad :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        REQUIRE_THROWS_AS(planFor(source, DevelopState{presence(bad, 0.0F, 0.0F)}),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(planFor(source, DevelopState{presence(0.0F, 0.0F, bad)}),
                          std::invalid_argument);
    }
    // Out of range is clamped, as every other setting (ADR 008).
    REQUIRE(planFor(source, DevelopState{presence(250.0F, 0.0F, 0.0F)}).presence.texture == 1.0F);
    REQUIRE_THROWS_AS(presencePlanFor({.clarity = 10.0F}, workingEncoding, 0.0, {8, 8}),
                      std::invalid_argument);
}
