#include "Lens.h"
#include "LensfunSource.h"
#include "support/LensFixtures.h"
#include "support/Lensfun.h"
#include "support/RowBandLimit.h"
#include "support/TempDir.h"

#include <ImageBuffer.h>
#include <LensCorrection.h>
#include <LensMatch.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <numbers>
#include <random>
#include <variant>
#include <vector>

using namespace arraw;
using Catch::Approx;

namespace {

using Radial = std::function<double(double)>;

/// @brief Builds a table from a function of the normalised radius.
std::vector<float> tableOf(const Radial& function) {
    std::vector<float> table(lensTableNodes);
    for (std::size_t node = 0; node < table.size(); ++node) {
        table[node] = static_cast<float>(function(static_cast<double>(node) / (table.size() - 1)));
    }
    return table;
}

/// @brief Builds a correction by hand, from functions of the normalised radius.
LensCorrection byHand(double aspect, const Radial& distortion = nullptr,
                      const Radial& red = nullptr, const Radial& blue = nullptr,
                      const Radial& vignetting = nullptr, double centerU = 0.5,
                      double centerV = 0.5) {
    LensCorrection correction;
    correction.aspect = aspect;
    correction.centerU = centerU;
    correction.centerV = centerV;
    if (distortion) {
        correction.distortion = tableOf(distortion);
    }
    if (red && blue) {
        correction.redScale = tableOf(red);
        correction.blueScale = tableOf(blue);
    }
    if (vignetting) {
        correction.vignetting = tableOf(vignetting);
    }
    return withFillScale(std::move(correction));
}

/// @brief Fills a float RGBA image from a function of the pixel centre's fractions.
ImageBuffer imageOf(ImageSize size,
                    const std::function<std::array<float, 4>(double, double)>& scene) {
    ImageBuffer image(size, workingFormat, workingEncoding);
    auto samples = image.samples<float>();
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const auto pixel = scene((x + 0.5) / size.width, (y + 0.5) / size.height);
            std::ranges::copy(pixel,
                              samples.begin() + (static_cast<std::size_t>(y) * size.width + x) * 4);
        }
    }
    return image;
}

float at(const ImageBuffer& image, std::uint32_t x, std::uint32_t y, int channel) {
    return image.samples<float>()[(static_cast<std::size_t>(y) * image.size().width + x) * 4 +
                                  static_cast<std::size_t>(channel)];
}

bool sameBytes(const ImageBuffer& a, const ImageBuffer& b) {
    return a.size() == b.size() && std::ranges::equal(a.bytes(), b.bytes());
}

double gaussian(double x, double center, double sigma) {
    return std::exp(-(x - center) * (x - center) / (2.0 * sigma * sigma));
}

constexpr double barrel(double r) {
    return 1.0 - 0.08 * r * r;
}

constexpr double pincushion(double r) {
    return 1.0 + 0.10 * r * r;
}

} // namespace

TEST_CASE("A radial table is read between its nodes", "[lens]") {
    const auto table = tableOf([](double r) { return 2.0 + r; });
    CHECK(tableValue(table, 0.0) == Approx(2.0));
    CHECK(tableValue(table, 1.0) == Approx(3.0));
    CHECK(tableValue(table, 0.37) == Approx(2.37).margin(1e-6));
    // Beyond the farthest corner the last value holds, and below zero the first.
    CHECK(tableValue(table, 1.5) == Approx(3.0));
    CHECK(tableValue(table, -0.5) == Approx(2.0));
}

TEST_CASE("A correction with no table is inactive and maps a point to itself", "[lens]") {
    const LensCorrection none;
    CHECK_FALSE(none.active());
    const SensorPoint point = sensorFrom(none, {0.3, 0.7});
    CHECK(point.u == 0.3F);
    CHECK(point.v == 0.7F);
    const auto back = correctedFrom(none, {0.3F, 0.7F});
    REQUIRE(back);
    CHECK(back->u == Approx(0.3));
    CHECK(withFillScale(LensCorrection{}).fill == 1.0);

    LensCorrection gainOnly;
    gainOnly.vignetting = tableOf([](double) { return 1.5; });
    CHECK(gainOnly.active());
    CHECK(sensorFrom(gainOnly, {0.2, 0.9}).v == 0.9F);
}

TEST_CASE("The fill scale is one for a barrel and more for a pincushion", "[lens]") {
    CHECK(fillScaleOf(byHand(1.5, barrel), 1.5) == Approx(1.0));
    const LensCorrection pin = byHand(1.5, pincushion);
    const double fill = fillScaleOf(pin, 1.5);
    CHECK(fill > 1.05);
    CHECK(pin.fill == fill);

    // The frame's corners are the tightest points of a centred pincushion: at the fill the
    // corner lands exactly on the sensor frame's corner.
    const SensorPoint corner = sensorFrom(pin, {1.0, 1.0});
    const double tightest = std::max(corner.u, corner.v);
    CHECK(tightest == Approx(1.0).margin(1e-4));
    // And no boundary point of the corrected frame reads outside the sensor frame.
    for (int step = 0; step <= 100; ++step) {
        const double along = step / 100.0;
        for (const auto position : {CorrectedPosition{along, 0.0}, CorrectedPosition{along, 1.0},
                                    CorrectedPosition{0.0, along}, CorrectedPosition{1.0, along}}) {
            const SensorPoint sensor = sensorFrom(pin, position);
            CHECK(sensor.u >= -1e-5F);
            CHECK(sensor.u <= 1.00001F);
            CHECK(sensor.v >= -1e-5F);
            CHECK(sensor.v <= 1.00001F);
        }
    }
    // The zoom is the least that does: a hair less and the corner reads outside.
    LensCorrection less = pin;
    less.fill = fill * 0.995;
    CHECK(std::max(sensorFrom(less, {1.0, 1.0}).u, sensorFrom(less, {1.0, 1.0}).v) > 1.0F);
}

TEST_CASE("The fill scale follows an off-centre optical centre", "[lens]") {
    const LensCorrection centred = byHand(1.5, pincushion);
    const LensCorrection shifted = byHand(1.5, pincushion, nullptr, nullptr, nullptr, 0.55, 0.45);
    // The farthest corner binds the zoom, wherever the centre is, and it maps along its own ray, so
    // the zoom is the same; what the centre changes is every other point of the boundary.
    CHECK(shifted.fill == Approx(centred.fill).epsilon(1e-6));
    // Still tight against the frame, wherever the centre is.
    double worst = 0.0;
    for (int step = 0; step <= 200; ++step) {
        const double along = step / 200.0;
        for (const auto position : {CorrectedPosition{along, 0.0}, CorrectedPosition{along, 1.0},
                                    CorrectedPosition{0.0, along}, CorrectedPosition{1.0, along}}) {
            const SensorPoint sensor = sensorFrom(shifted, position);
            worst = std::max({worst, static_cast<double>(sensor.u), static_cast<double>(sensor.v)});
            CHECK(sensor.u >= -1e-5F);
            CHECK(sensor.v >= -1e-5F);
            CHECK(sensor.u <= 1.00001F);
            CHECK(sensor.v <= 1.00001F);
        }
    }
    CHECK(worst == Approx(1.0).margin(1e-3));
}

TEST_CASE("The corrected position of a sensor point inverts the sensor point", "[lens]") {
    // 1e-4 px at the width of a thousand: the sensor point is a float.
    constexpr double tolerance = 1.0e-4 / 1000.0;
    const std::vector<LensCorrection> corrections = {
        byHand(1.5, barrel), byHand(1.5, pincushion),
        byHand(
            1.5, [](double r) { return 1.0 + 0.06 * r - 0.1 * r * r * r; }, nullptr, nullptr,
            nullptr, 0.52, 0.47),
        byHand(1.0, pincushion, nullptr, nullptr, nullptr, 0.4, 0.55)};
    for (const LensCorrection& correction : corrections) {
        for (double u = -0.2; u <= 1.2; u += 0.05) {
            for (double v = -0.2; v <= 1.2; v += 0.05) {
                const SensorPoint sensor = sensorFrom(correction, {u, v});
                const auto back = correctedFrom(correction, sensor);
                REQUIRE(back);
                CHECK(std::abs(back->u - u) < tolerance * 4.0);
                CHECK(std::abs(back->v - v) < tolerance * 4.0);
            }
        }
    }
}

TEST_CASE("A distortion that cannot be undone says so", "[lens]") {
    // The radius times this scale peaks at 0.125 and falls: a point at 0.5 is on no ray.
    const LensCorrection folded = byHand(1.0, [](double r) { return 1.0 - 2.0 * r; });
    CHECK_FALSE(correctedFrom(folded, {0.5F, 1.0F}));
    // A point it does reach is found, on the first ray that does: the smallest radius.
    const auto reached = correctedFrom(folded, {0.5F, 0.55F});
    REQUIRE(reached);
    CHECK(reached->v < 0.5 + 0.25 * 0.7071 + 1e-6);
}

TEST_CASE("A plan whose tables are all one returns the source's bits", "[lens]") {
    LensCorrection ones = byHand(
        1.5, [](double) { return 1.0; }, [](double) { return 1.0; }, [](double) { return 1.0; },
        [](double) { return 1.0; });
    REQUIRE(ones.fill == 1.0);
    std::mt19937 random(7);
    std::uniform_real_distribution<float> values(-0.2F, 3.0F);
    ImageBuffer source = imageOf({97, 61}, [&](double, double) {
        return std::array<float, 4>{values(random), values(random), values(random), 1.0F};
    });
    const ImageBuffer corrected = applyLens(source, LensPlan{ones});
    CHECK(sameBytes(source, corrected));

    // Each correction alone, too, and an inactive plan, which returns a copy.
    for (const bool distortion : {false, true}) {
        LensCorrection one;
        one.aspect = 1.5;
        if (distortion) {
            one.distortion = tableOf([](double) { return 1.0; });
        } else {
            one.redScale = tableOf([](double) { return 1.0; });
            one.blueScale = tableOf([](double) { return 1.0; });
        }
        CHECK(sameBytes(source, applyLens(source, LensPlan{one})));
    }
    CHECK(sameBytes(source, applyLens(source, LensPlan{})));
}

TEST_CASE("A falloff becomes flat and values above one survive", "[lens]") {
    const Radial gain = [](double r) { return 1.0 + 0.8 * r * r; };
    const LensCorrection falloff = byHand(1.5, nullptr, nullptr, nullptr, gain);
    const ImageSize size{120, 80};

    // A flat scene as a lens with this falloff would record it.
    const ImageBuffer dark = imageOf(size, [&](double u, double v) {
        const SensorPoint point{static_cast<float>(u), static_cast<float>(v)};
        const auto value =
            static_cast<float>(0.4 / tableValue(falloff.vignetting, radiusOf(falloff, point)));
        return std::array<float, 4>{value, value, value, 1.0F};
    });
    const ImageBuffer flat = applyLens(dark, LensPlan{falloff});
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                REQUIRE(at(flat, x, y, channel) == Approx(0.4F).margin(1e-4));
            }
            REQUIRE(at(flat, x, y, 3) == 1.0F);
        }
    }

    // A bright scene is brightened past one, not clipped to it.
    const ImageBuffer bright =
        imageOf(size, [](double, double) { return std::array<float, 4>{0.9F, 0.9F, 0.9F, 1.0F}; });
    const ImageBuffer lifted = applyLens(bright, LensPlan{falloff});
    float largest = 0.0F;
    for (std::uint32_t x = 0; x < size.width; ++x) {
        largest = std::max(largest, at(lifted, x, 0, 1));
    }
    CHECK(largest > 1.4F);
}

TEST_CASE("A lens straightens a grid, barrel or pincushion", "[lens]") {
    const ImageSize size{480, 320};
    constexpr double spacing = 40.0;
    constexpr double sigma = 2.0;
    for (const auto& [name, shape] :
         {std::pair<const char*, Radial>{"barrel", barrel}, {"pincushion", pincushion}}) {
        CAPTURE(name);
        const LensCorrection correction = byHand(1.5, shape);

        // The grid as the lens records it: each sensor pixel shows the scene at the corrected
        // position the pixel corresponds to, so that correcting must undo exactly that.
        const auto grid = [&](double along, double extent) {
            const double position = along * extent - 0.5;
            double best = 0.0;
            for (double line = spacing; line < extent; line += spacing) {
                best = std::max(best, gaussian(position, line - 0.5, sigma));
            }
            return static_cast<float>(best);
        };
        const ImageBuffer recorded = imageOf(size, [&](double u, double v) {
            const auto position =
                correctedFrom(correction, {static_cast<float>(u), static_cast<float>(v)});
            if (!position) {
                return std::array<float, 4>{0, 0, 0, 1};
            }
            const float value =
                std::max(grid(position->u, size.width), grid(position->v, size.height));
            return std::array<float, 4>{value, value, value, 1.0F};
        });
        const ImageBuffer straight = applyLens(recorded, LensPlan{correction});

        // Every vertical line is at its column, row after row; every horizontal line at its row.
        double worst = 0.0;
        for (int line = 3; line <= 8; ++line) {
            const double wanted = line * spacing - 0.5;
            for (std::uint32_t y = 60; y < 260; y += 7) {
                // Rows between the horizontal lines only: on one, the whole row is bright.
                if (std::abs(std::fmod(y + 0.5, spacing) - spacing / 2.0) > spacing / 2.0 - 10.0) {
                    continue;
                }
                double weight = 0.0;
                double moment = 0.0;
                for (auto x = static_cast<std::uint32_t>(wanted) - 8; x <= wanted + 8; ++x) {
                    // Rows between horizontal lines only, so a crossing does not bias the centroid.
                    const double value = at(straight, x, y, 1);
                    weight += value;
                    moment += value * x;
                }
                if (weight > 0.5) {
                    worst = std::max(worst, std::abs(moment / weight - wanted));
                }
            }
        }
        for (int line = 2; line <= 7; ++line) {
            const double wanted = line * spacing - 0.5;
            for (std::uint32_t x = 100; x < 380; x += 9) {
                if (std::abs(std::fmod(x + 0.5, spacing) - spacing / 2.0) > spacing / 2.0 - 10.0) {
                    continue;
                }
                double weight = 0.0;
                double moment = 0.0;
                for (auto y = static_cast<std::uint32_t>(wanted) - 8; y <= wanted + 8; ++y) {
                    const double value = at(straight, x, y, 1);
                    weight += value;
                    moment += value * y;
                }
                if (weight > 0.5) {
                    worst = std::max(worst, std::abs(moment / weight - wanted));
                }
            }
        }
        CHECK(worst < 0.05);
    }
}

TEST_CASE("A colour-split edge is realigned", "[lens]") {
    const ImageSize size{400, 200};
    const LensCorrection correction =
        byHand(2.0, nullptr, [](double) { return 1.01; }, [](double) { return 0.99; });
    const double edge = 0.8 * size.width - 0.5; // where green and the corrected edge are
    const double centre = 0.5 * size.width - 0.5;
    // Each channel records the edge where its own scale puts it.
    const double redEdge = centre + (edge - centre) * 1.01;
    const double blueEdge = centre + (edge - centre) * 0.99;
    const auto step = [](double x, double at) {
        return static_cast<float>(0.5 * (1.0 + std::erf((x - at) / (1.5 * std::numbers::sqrt2))));
    };
    const ImageBuffer recorded = imageOf(size, [&](double u, double) {
        const double x = u * size.width - 0.5;
        return std::array<float, 4>{step(x, redEdge), step(x, edge), step(x, blueEdge), 1.0F};
    });
    const ImageBuffer aligned = applyLens(recorded, LensPlan{correction});
    for (int channel = 0; channel < 3; ++channel) {
        CAPTURE(channel);
        // The 50% crossing, by linear interpolation between the two pixels around it.
        double crossing = -1.0;
        for (std::uint32_t x = 200; x + 1 < size.width; ++x) {
            const float a = at(aligned, x, 100, channel);
            const float b = at(aligned, x + 1, 100, channel);
            if (a < 0.5F && b >= 0.5F) {
                crossing = x + (0.5 - a) / (b - a);
                break;
            }
        }
        CHECK(crossing == Approx(edge).margin(0.05));
    }
}

TEST_CASE("The kernel reads between pixels better than bilinear", "[lens]") {
    // A gentle zoom puts samples at every fraction of a pixel; a sinusoid of a fifth of the
    // sampling rate is what each kernel is asked to reconstruct.
    const ImageSize size{256, 64};
    LensCorrection zoom = byHand(4.0, [](double) { return 1.0037; });
    // Without the fill, so that what is read is the 0.37% about the centre and nothing more.
    zoom.fill = 1.0;
    const auto scene = [&](double x) {
        return 0.5F + 0.4F * std::sin(static_cast<float>(x * 2.0 * std::numbers::pi * 0.2));
    };
    const ImageBuffer source = imageOf(size, [&](double u, double) {
        const float value = scene(u * size.width - 0.5);
        return std::array<float, 4>{value, value, value, 1.0F};
    });
    const auto error = [&](detail::LensResampler resampler) {
        const ImageBuffer out = detail::applyLens(source, LensPlan{zoom}, resampler);
        double worst = 0.0;
        for (std::uint32_t x = 20; x < size.width - 20; ++x) {
            // The sensor position read for this pixel: about the centre, 0.37% farther out.
            const double centre = 0.5 * size.width - 0.5;
            const double read = centre + (x - centre) * 1.0037;
            worst = std::max(worst, std::abs(static_cast<double>(at(out, x, 32, 1)) - scene(read)));
        }
        return worst;
    };
    const double bilinear = error(detail::LensResampler::Bilinear);
    const double cubic = error(detail::LensResampler::CatmullRom);
    const double lanczos = error(detail::LensResampler::Lanczos3);
    INFO("bilinear " << bilinear << ", Catmull-Rom " << cubic << ", Lanczos-3 " << lanczos);
    CHECK(cubic < bilinear / 3.0);
    CHECK(lanczos < bilinear / 6.0);
    CHECK(cubic < 0.03);
}

TEST_CASE("The pass gives the same bits however its rows are split", "[lens]") {
    const LensCorrection correction = byHand(
        1.5, barrel, [](double r) { return 1.0 + 0.003 * r; },
        [](double r) { return 1.0 - 0.003 * r; }, [](double r) { return 1.0 + 0.5 * r * r; }, 0.52,
        0.48);
    std::mt19937 random(11);
    std::uniform_real_distribution<float> values(0.0F, 2.0F);
    const ImageBuffer source = imageOf({301, 201}, [&](double, double) {
        return std::array<float, 4>{values(random), values(random), values(random), 1.0F};
    });
    ImageBuffer single(source.size(), workingFormat, workingEncoding);
    {
        const test::ScopedRowBandLimit limit(1);
        single = applyLens(source, LensPlan{correction});
    }
    ImageBuffer banded(source.size(), workingFormat, workingEncoding);
    {
        const test::ScopedRowBandLimit limit(5);
        banded = applyLens(source, LensPlan{correction});
    }
    CHECK(sameBytes(single, banded));
}

TEST_CASE("The pass keeps what describes the source", "[lens]") {
    ImageBuffer source = imageOf(
        {40, 30}, [](double, double) { return std::array<float, 4>{0.5F, 0.5F, 0.5F, 1.0F}; });
    source.setPixelScale(2.0);
    const ImageBuffer out = applyLens(source, LensPlan{byHand(1.3333, barrel)});
    CHECK(out.size() == source.size());
    CHECK(out.pixelScale() == 2.0);
    CHECK(out.orientation() == source.orientation());
    CHECK(out.encoding() == source.encoding());

    const ImageBuffer wrong(ImageSize{4, 4}, PixelFormat::RgbaU16, workingEncoding);
    CHECK_THROWS_AS(applyLens(wrong, LensPlan{byHand(1.0, barrel)}), std::invalid_argument);
}

TEST_CASE("The result at half the size is the full-size result at half", "[lens]") {
    // A smooth scene, so what is compared is the mapping and not the sampling of detail.
    const auto scene = [](double u, double v) {
        const auto value = static_cast<float>(0.5 + 0.25 * std::sin(2.0 * std::numbers::pi * u) +
                                              0.2 * std::cos(std::numbers::pi * v * 1.5));
        return std::array<float, 4>{value, value * 0.9F, value * 0.8F, 1.0F};
    };
    const LensCorrection correction = byHand(
        1.5, pincushion, [](double r) { return 1.0 + 0.002 * r; },
        [](double r) { return 1.0 - 0.002 * r; }, [](double r) { return 1.0 + 0.6 * r * r; });

    // Even sizes: the half-size image is the full-size one averaged over 2x2.
    const ImageBuffer fullCorrected = applyLens(imageOf({240, 160}, scene), LensPlan{correction});
    const ImageBuffer halfCorrected = applyLens(imageOf({120, 80}, scene), LensPlan{correction});
    double worst = 0.0;
    for (std::uint32_t y = 0; y < 80; ++y) {
        for (std::uint32_t x = 0; x < 120; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const float mean = 0.25F * (at(fullCorrected, 2 * x, 2 * y, channel) +
                                            at(fullCorrected, 2 * x + 1, 2 * y, channel) +
                                            at(fullCorrected, 2 * x, 2 * y + 1, channel) +
                                            at(fullCorrected, 2 * x + 1, 2 * y + 1, channel));
                worst = std::max(
                    worst, std::abs(static_cast<double>(mean - at(halfCorrected, x, y, channel))));
            }
        }
    }
    CHECK(worst < 2.0e-3);

    // Odd sizes, as a half-size decode of an odd sensor gives (floor of half): the same frame, up
    // to half a half-size pixel off in registration at the far edge, which a looser bound allows.
    const ImageBuffer oddFull = applyLens(imageOf({241, 161}, scene), LensPlan{correction});
    const ImageBuffer oddHalf = applyLens(imageOf({120, 80}, scene), LensPlan{correction});
    double oddWorst = 0.0;
    for (std::uint32_t y = 0; y < 80; ++y) {
        for (std::uint32_t x = 0; x < 120; ++x) {
            const float mean =
                0.25F * (at(oddFull, 2 * x, 2 * y, 1) + at(oddFull, 2 * x + 1, 2 * y, 1) +
                         at(oddFull, 2 * x, 2 * y + 1, 1) + at(oddFull, 2 * x + 1, 2 * y + 1, 1));
            oddWorst =
                std::max(oddWorst, std::abs(static_cast<double>(mean - at(oddHalf, x, y, 1))));
        }
    }
    CHECK(oddWorst < 4.0e-2);
}

// ---------------------------------------------------------------------------------------------
// lensfun's models, sampled.

namespace {

LensMatch matchOf(const char* maker, const char* model, double crop, double focal) {
    LensMatch match;
    match.lens = {maker, model};
    match.cropFactor = crop;
    match.cropSource = CropSource::Camera;
    match.cameraListed = true;
    match.focalLength = focal;
    match.focalLengthRead = true;
    return match;
}

/// @brief One lens of the fixture at one focal length and aperture.
struct Case {
    const char* name;
    LensMatch match;
    double aperture;
};

std::vector<Case> cases() {
    return {{"the kernel's lens", matchOf("ArrawTest", "TestLens 50mm F2.8", 1.5, 50.0), 2.8},
            {"the zoom at its wide end",
             matchOf("Canon", "Canon EF-S 18-135mm f/3.5-5.6 IS USM", 1.6, 18.0), 3.5},
            {"the zoom between its calibrated focal lengths",
             matchOf("Canon", "Canon EF-S 18-135mm f/3.5-5.6 IS USM", 1.6, 50.0), 4.5},
            {"the zoom at its long end",
             matchOf("Canon", "Canon EF-S 18-135mm f/3.5-5.6 IS USM", 1.6, 135.0), 5.6},
            {"the EF prime", matchOf("Canon", "Canon EF 50mm f/1.8 STM", 1.6, 50.0), 1.8}};
}

/// @brief Where a corrected-frame pixel's channels read, by the tables, in index coordinates.
std::array<std::array<double, 2>, 3> tablePositions(const LensCorrection& correction,
                                                    ImageSize size, double x, double y) {
    const CorrectedPosition output{(x + 0.5) / size.width, (y + 0.5) / size.height};
    const SensorPoint green = sensorFrom(correction, output);
    const double radius = radiusOf(correction, green);
    const double redScale =
        correction.hasLateralCa() ? tableValue(correction.redScale, radius) : 1.0;
    const double blueScale =
        correction.hasLateralCa() ? tableValue(correction.blueScale, radius) : 1.0;
    // Sensor fractions to index coordinates, the colours about the centre.
    const double gx = green.u * size.width - 0.5;
    const double gy = green.v * size.height - 0.5;
    const double cx = correction.centerU * size.width - 0.5;
    const double cy = correction.centerV * size.height - 0.5;
    return {{{cx + (gx - cx) * redScale, cy + (gy - cy) * redScale},
             {gx, gy},
             {cx + (gx - cx) * blueScale, cy + (gy - cy) * blueScale}}};
}

/// @brief Probes where lensfun's geometry has its fixed point, which is its optical centre.
std::array<double, 2> fixedPointOf(const detail::LensfunReference& reference, ImageSize size) {
    double x = (size.width - 1.0) / 2.0;
    double y = (size.height - 1.0) / 2.0;
    for (int round = 0; round < 60; ++round) {
        const auto displacement = [&](double px, double py) {
            const auto green = reference.sourcePositions(px, py)[1];
            return std::array<double, 2>{green[0] - px, green[1] - py};
        };
        const auto d = displacement(x, y);
        const auto dx = displacement(x + 8.0, y);
        const auto dy = displacement(x, y + 8.0);
        const double a = (dx[0] - d[0]) / 8.0;
        const double b = (dy[0] - d[0]) / 8.0;
        const double c = (dx[1] - d[1]) / 8.0;
        const double e = (dy[1] - d[1]) / 8.0;
        const double det = a * e - b * c;
        x -= (d[0] * e - d[1] * b) / det;
        y -= (-d[0] * c + d[1] * a) / det;
    }
    return {x, y};
}

/// @brief Points where lensfun and the tables are compared: corners, edge midpoints and a scatter.
std::vector<std::array<double, 2>> checkpoints(ImageSize size) {
    const double right = size.width - 1.0;
    const double bottom = size.height - 1.0;
    std::vector<std::array<double, 2>> points = {
        {0, 0},          {right, 0},          {0, bottom},
        {right, bottom}, {right / 2, 0},      {right / 2, bottom},
        {0, bottom / 2}, {right, bottom / 2}, {right / 2, bottom / 2}};
    std::mt19937 random(5);
    std::uniform_real_distribution<double> across(0.0, right);
    std::uniform_real_distribution<double> down(0.0, bottom);
    for (int count = 0; count < 40; ++count) {
        points.push_back({across(random), down(random)});
    }
    return points;
}

} // namespace

TEST_CASE("The tables hold lensfun's answer to under 0.05 pixels", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const ImageSize size{6000, 4000};
    constexpr LensKinds all{true, true, true};

    for (const Case& one : cases()) {
        CAPTURE(one.name);
        LensFacts facts;
        facts.aperture = one.aperture;
        const detail::LensfunReference reference(database, one.match, facts, size);
        REQUIRE(reference.valid());

        for (const std::size_t nodes : {std::size_t{65}, lensTableNodes, std::size_t{257}}) {
            CAPTURE(nodes);
            const auto sampled = sampleLensfun(database, one.match, facts, size, all, nodes);
            REQUIRE(sampled);
            REQUIRE_FALSE(sampled->distortion.empty());
            REQUIRE(sampled->hasLateralCa());
            REQUIRE_FALSE(sampled->vignetting.empty());

            double worst = 0.0;
            double worstGain = 0.0;
            const double centreX = sampled->centerU * size.width - 0.5;
            const double centreY = sampled->centerV * size.height - 0.5;
            for (const auto& [x, y] : checkpoints(size)) {
                // lensfun is asked for no zoom: it is given the point the zoom puts there.
                const auto theirs =
                    reference.sourcePositions(centreX + (x - centreX) / sampled->fill,
                                              centreY + (y - centreY) / sampled->fill);
                const auto ours = tablePositions(*sampled, size, x, y);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    worst = std::max(worst, std::hypot(theirs[channel][0] - ours[channel][0],
                                                       theirs[channel][1] - ours[channel][1]));
                }
                // Vignetting is read at the sensor pixel lensfun's geometry finds.
                const SensorPoint sensor{static_cast<float>((theirs[1][0] + 0.5) / size.width),
                                         static_cast<float>((theirs[1][1] + 0.5) / size.height)};
                worstGain =
                    std::max(worstGain,
                             std::abs(reference.gainAt(theirs[1][0], theirs[1][1]) -
                                      tableValue(sampled->vignetting, radiusOf(*sampled, sensor))));
            }
            INFO(one.name << ", " << nodes << " nodes: worst position error " << worst
                          << " px, worst gain error " << worstGain);
            if (nodes >= lensTableNodes) {
                CHECK(worst < 0.05);
                CHECK(worstGain < 1.0e-3);
            }
            if (nodes == lensTableNodes) {
                WARN(one.name << ": " << nodes << " nodes, position " << worst << " px, gain "
                              << worstGain);
            }
            if (std::getenv("ARRAW_LENS_NODE_SWEEP") != nullptr) {
                WARN(one.name << ": " << nodes << " nodes, position " << worst << " px, gain "
                              << worstGain);
            }
        }
    }
}

TEST_CASE("The tables are sampled about lensfun's calibrated centre", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    for (const ImageSize size :
         {ImageSize{6000, 4000}, ImageSize{4000, 6000}, ImageSize{3001, 2001}}) {
        CAPTURE(size.width, size.height);
        const LensMatch match = matchOf("ArrawTest", "TestLens 50mm F2.8", 1.5, 50.0);
        LensFacts facts;
        facts.aperture = 2.8;
        const detail::LensfunReference reference(database, match, facts, size);
        const auto sampled = sampleLensfun(database, match, facts, size, {true, true, true});
        REQUIRE(sampled);

        // Where lensfun's own geometry stands still, found by probing it, is the centre we use,
        // and it is not the middle of the frame.
        const auto fixed = fixedPointOf(reference, size);
        const double centreX = sampled->centerU * size.width - 0.5;
        const double centreY = sampled->centerV * size.height - 0.5;
        CHECK(centreX == Approx(fixed[0]).margin(0.05));
        CHECK(centreY == Approx(fixed[1]).margin(0.05));
        CHECK(std::abs(centreX - (size.width - 1.0) / 2.0) > 10.0 * size.width / 6000.0);
    }
}

TEST_CASE("A zoom is interpolated between its calibrated focal lengths", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const ImageSize size{3000, 2000};
    const auto at = [&](double focal, double aperture) {
        LensFacts facts;
        facts.aperture = aperture;
        const auto sampled = sampleLensfun(
            database, matchOf("Canon", "Canon EF-S 18-135mm f/3.5-5.6 IS USM", 1.6, focal), facts,
            size, {true, true, true});
        REQUIRE(sampled);
        return *sampled;
    };
    const LensCorrection wide = at(18.0, 3.5);
    const LensCorrection middle = at(50.0, 4.5);
    const LensCorrection tele = at(135.0, 5.6);
    // At the corner the distortion runs from the wide end's to the tele end's, and the middle
    // lies strictly between them: neither end's profile is used for it.
    const double w = wide.distortion.back();
    const double m = middle.distortion.back();
    const double t = tele.distortion.back();
    CHECK(w != Approx(t).epsilon(1e-3));
    CHECK(m > std::min(w, t));
    CHECK(m < std::max(w, t));
    CHECK(middle.distortionFrom->focalLength == 50.0);
    CHECK(wide.vignetting.back() > tele.vignetting.back());
}

TEST_CASE("What a profile assumed is marked", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const LensMatch match = matchOf("Canon", "Canon EF-S 18-135mm f/3.5-5.6 IS USM", 1.6, 50.0);

    LensFacts told;
    told.aperture = 4.5;
    told.focusDistance = 3.0;
    const auto known = sampleLensfun(database, match, told, {600, 400}, {true, true, true});
    REQUIRE(known);
    REQUIRE(known->vignettingFrom);
    CHECK(known->vignettingFrom->aperture == 4.5);
    CHECK(known->vignettingFrom->apertureRead);
    CHECK(known->vignettingFrom->distance == 3.0);
    CHECK(known->vignettingFrom->distanceRead);
    CHECK(known->vignettingFrom->focalLengthRead);
    CHECK(known->vignettingFrom->lens == match.lens);
    CHECK(known->vignettingFrom->cropFactor == 1.6);
    CHECK(known->vignettingFrom->focalLength == 50.0);
    CHECK(known->vignettingFrom->database == database.stamp());

    const auto guessed =
        sampleLensfun(database, match, LensFacts{}, {600, 400}, {true, true, true});
    REQUIRE(guessed);
    CHECK(guessed->vignettingFrom->aperture == assumedAperture);
    CHECK_FALSE(guessed->vignettingFrom->apertureRead);
    CHECK(guessed->vignettingFrom->distance == assumedDistance);
    CHECK_FALSE(guessed->vignettingFrom->distanceRead);
    CHECK(guessed->distortionFrom);
    CHECK(guessed->lateralCaFrom);
}

TEST_CASE("Lenses that share a name are sampled by the calibration that fits the sensor",
          "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const test::TempDir both;
    const test::TempDir fullOnly;
    const test::TempDir cropOnly;
    test::writeDuplicateDatabase(both.path(), {{1.0, -0.05}, {1.6, 0.05}});
    test::writeDuplicateDatabase(fullOnly.path(), {{1.0, -0.05}});
    test::writeDuplicateDatabase(cropOnly.path(), {{1.6, 0.05}});
    const LensDatabase twice = LensDatabase::fromDirectory(both.path());
    const LensDatabase full = LensDatabase::fromDirectory(fullOnly.path());
    const LensDatabase crop = LensDatabase::fromDirectory(cropOnly.path());
    const ImageSize size{600, 400};
    const auto sampled = [&](const LensDatabase& database, double cropFactor) {
        return sampleLensfun(database, matchOf("Test", "Test Dup 50mm", cropFactor, 50.0),
                             LensFacts{}, size, {true, false, false});
    };

    // The camera is a 1.6 crop: the lens measured on 1.6 is the one, though the other is first.
    const auto onCrop = sampled(twice, 1.6);
    REQUIRE(onCrop);
    CHECK(onCrop->distortion == sampled(crop, 1.6)->distortion);
    CHECK(onCrop->distortion != sampled(full, 1.6)->distortion);
    // A full-frame camera takes the full-frame calibration.
    const auto onFull = sampled(twice, 1.0);
    REQUIRE(onFull);
    CHECK(onFull->distortion == sampled(full, 1.0)->distortion);
    // Matching and sampling agree end to end, for the recorded lens as well.
    LensFacts facts;
    facts.cameraMaker = "Test";
    facts.cameraModel = "Body";
    facts.lensNames = {"Test Dup 50mm"};
    facts.focalLength = 50.0;
    const auto matched = matchLens(facts, twice);
    REQUIRE(std::holds_alternative<LensMatch>(matched));
    CHECK(sampleLensfun(twice, std::get<LensMatch>(matched), facts, size, {true, false, false})
              ->distortion == onCrop->distortion);
}

TEST_CASE("Only the wanted corrections are sampled, and only those the lens has",
          "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const ImageSize size{600, 400};

    const LensMatch prime = matchOf("Canon", "Canon EF 50mm f/1.8 STM", 1.6, 50.0);
    const auto distortionOnly =
        sampleLensfun(database, prime, LensFacts{}, size, {true, false, false});
    REQUIRE(distortionOnly);
    CHECK_FALSE(distortionOnly->distortion.empty());
    CHECK_FALSE(distortionOnly->hasLateralCa());
    CHECK(distortionOnly->vignetting.empty());
    CHECK_FALSE(distortionOnly->vignettingFrom);

    const auto vignettingOnly =
        sampleLensfun(database, prime, LensFacts{}, size, {false, true, false});
    REQUIRE(vignettingOnly);
    CHECK(vignettingOnly->distortion.empty());
    CHECK(vignettingOnly->fill == 1.0);

    // The Tamron has a distortion calibration and nothing else.
    const LensMatch tamron = matchOf("Tamron", "Tamron SP 24-70mm f/2.8 Di VC USD", 1.5, 35.0);
    const auto partial = sampleLensfun(database, tamron, LensFacts{}, size, {true, true, true});
    REQUIRE(partial);
    CHECK_FALSE(partial->distortion.empty());
    CHECK_FALSE(partial->hasLateralCa());
    CHECK(partial->vignetting.empty());

    const auto none = sampleLensfun(database, prime, LensFacts{}, size, {});
    REQUIRE(none);
    CHECK_FALSE(none->active());

    // A lens the database does not list resolves to nothing.
    CHECK_FALSE(sampleLensfun(database, matchOf("Nobody", "Not A Lens", 1.0, 50.0), LensFacts{},
                              size, {true, true, true}));
}

TEST_CASE("The fill scale is the same at every resolution", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const LensMatch match = matchOf("ArrawTest", "TestLens 35mm F4 Pincushion", 1.5, 35.0);
    std::vector<double> fills;
    for (const ImageSize size :
         {ImageSize{6000, 4000}, ImageSize{3000, 2000}, ImageSize{256, 171}}) {
        const auto sampled =
            sampleLensfun(database, match, LensFacts{}, size, {true, false, false});
        REQUIRE(sampled);
        fills.push_back(sampled->fill);
        // And it keeps the pass inside the sensor frame at that size.
        for (int step = 0; step <= 50; ++step) {
            const double along = step / 50.0;
            const SensorPoint corner = sensorFrom(*sampled, {along, 1.0});
            CHECK(corner.u <= 1.0001F);
            CHECK(corner.v <= 1.0001F);
        }
    }
    CHECK(fills[0] > 1.05);
    CHECK(fills[1] == Approx(fills[0]).epsilon(2e-3));
    CHECK(fills[2] == Approx(fills[0]).epsilon(2e-3));
}

TEST_CASE("A lensfun-resolved correction corrects an image end to end", "[lensfun][lens]") {
    ARRAW_REQUIRE_LENSFUN_OR_SKIP();
    const LensDatabase& database = test::fixtureDatabase();
    const ImageSize size{300, 200};
    const LensMatch match = matchOf("ArrawTest", "TestLens 50mm F2.8", 1.5, 50.0);
    LensFacts facts;
    facts.aperture = 2.8;
    const auto sampled = sampleLensfun(database, match, facts, size, {true, true, true});
    REQUIRE(sampled);
    const ImageBuffer flat =
        imageOf(size, [](double, double) { return std::array<float, 4>{0.3F, 0.3F, 0.3F, 1.0F}; });
    const ImageBuffer corrected = applyLens(flat, LensPlan{*sampled});
    // The falloff of the lens is lifted, most in the corner, which is brighter than the middle.
    const float middle = at(corrected, 150, 100, 1);
    const float corner = at(corrected, 0, 0, 1);
    CHECK(middle == Approx(0.3F).margin(0.01));
    CHECK(corner > middle * 1.2F);
    CHECK(std::isfinite(corner));
}

/// Hidden: the Lens pass's cost, and the tables' error against node count (ADR 050, step 4).
/// Run with `arraw-tests "[.lensbench]"`; it prints, and asserts nothing.
TEST_CASE("The cost of the lens pass", "[.lensbench]") {
    const test::ScopedRowBandLimit limit(8);
    const auto run = [](ImageSize size, const LensCorrection& correction,
                        detail::LensResampler resampler) {
        const ImageBuffer source = imageOf(size, [](double u, double v) {
            return std::array<float, 4>{static_cast<float>(u), static_cast<float>(v), 0.5F, 1.0F};
        });
        const auto start = std::chrono::steady_clock::now();
        const ImageBuffer out = detail::applyLens(source, LensPlan{correction}, resampler);
        (void)out;
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };
    const Radial scale = [](double r) { return 1.0 - 0.06 * r * r; };
    const Radial red = [](double r) { return 1.0 + 0.001 * r; };
    const Radial blue = [](double r) { return 1.0 - 0.001 * r; };
    const Radial gain = [](double r) { return 1.0 + 0.8 * r * r; };
    struct Kind {
        const char* name;
        LensCorrection correction;
    };
    const std::vector<Kind> kinds = {
        {"distortion", byHand(1.5, scale)},
        {"distortion+CA", byHand(1.5, scale, red, blue)},
        {"all three", byHand(1.5, scale, red, blue, gain)},
        {"CA alone", byHand(1.5, nullptr, red, blue)},
        {"vignetting alone", byHand(1.5, nullptr, nullptr, nullptr, gain)}};
    for (const auto& [sizeName, size] : {std::pair<const char*, ImageSize>{"24 MP", {6000, 4000}},
                                         {"preview 1500x1000", {1500, 1000}},
                                         {"thumbnail 256x171", {256, 171}}}) {
        for (const auto& kind : kinds) {
            for (const auto& [name, resampler] :
                 {std::pair<const char*, detail::LensResampler>{"bilinear",
                                                                detail::LensResampler::Bilinear},
                  {"Catmull-Rom", detail::LensResampler::CatmullRom},
                  {"Lanczos-3", detail::LensResampler::Lanczos3}}) {
                WARN(sizeName << ", " << kind.name << ", " << name << ": "
                              << run(size, kind.correction, resampler) << " ms");
            }
        }
    }
    SUCCEED();
}
