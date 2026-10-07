#include "GpuContext.h"
#include "GpuDevelop.h"
#include "GpuPlan.h"
#include "GpuTesting.h"
#include "ProcessingPlan.h"

#include <CurveHistogram.h>
#include <Develop.h>
#include <DevelopSettings.h>
#include <DevelopState.h>
#include <GeometrySettings.h>
#include <ImageBuffer.h>
#include <ImageImport.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <variant>

#ifndef ARRAW_TEST_DATA_DIR
#error "ARRAW_TEST_DATA_DIR must name the fixture directory; see tests/CMakeLists.txt"
#endif

using namespace arraw;
using namespace arraw::test;

namespace {

/// @brief Largest colour error of the tapped chain followed by geometry and a resize.
///
/// The same passes as an end-to-end render, compared in linear light, where
/// the parity tolerances were measured; the encoding is one host function both
/// backends share.
constexpr double tapTolerance = pointwiseRelativeTolerance + resampleTolerance;

/// @brief Largest share of pixels whose bin may differ between the backends, per channel.
///
/// A value within the tolerance above can still cross a bin edge; each such
/// pixel moves one count between two neighbouring bins, which costs two in
/// the distance below. Measured on lavapipe over the cases below: no count
/// moved, with a worst colour error of 1.0e-5. The allowance is for drivers
/// whose `pow` is less exact (see illConditionedRelativeTolerance).
constexpr double binTolerance = 0.01;

/// @brief Loads a fixture with its own encoding and orientation.
ImageBuffer fixtureImage(const char* name) {
    return loadImage(std::filesystem::path(ARRAW_TEST_DATA_DIR) / name);
}

/// @brief Decodes a perceptual sample back to linear light, for comparing in the space measured.
ImageBuffer decoded(const ImageBuffer& tapped) {
    REQUIRE(std::get<NamedEncoding>(tapped.encoding()) == perceptualEncoding);
    ImageBuffer linear(tapped.size(), PixelFormat::RgbaF32, workingEncoding);
    const auto in = tapped.samples<float>();
    const auto out = linear.samples<float>();
    for (std::size_t index = 0; index < in.size(); ++index) {
        out[index] = index % 4 == 3 ? in[index] : fromPerceptualSigned(in[index]);
    }
    return linear;
}

/// @brief Sums how far two histograms' bins differ, over one channel.
std::uint64_t binDistance(const CurveHistogram::Bins& first, const CurveHistogram::Bins& second) {
    std::uint64_t distance = 0;
    for (std::size_t bin = 0; bin < first.size(); ++bin) {
        distance += first[bin] > second[bin] ? first[bin] - second[bin] : second[bin] - first[bin];
    }
    return distance;
}

/// @brief Builds settings that move every stage before and after the tap.
DevelopSettings busy(GeometrySettings geometry) {
    DevelopSettings settings;
    settings.tone = {0.4F, 30.0F, 30.0F, -30.0F, 10.0F, 20.0F, 70.0F};
    settings.toneCurve.luma.points = {{0.0F, 0.1F}, {0.5F, 0.7F}, {1.0F, 0.9F}};
    settings.toneCurve.green.points = {{0.0F, 0.0F}, {0.3F, 0.5F}, {1.0F, 1.0F}};
    settings.color.saturation = 30.0F;
    settings.geometry = geometry;
    return settings;
}

} // namespace

TEST_CASE("The curve input tap is the pointwise probe after Basic Tone", "[gpu][sample]") {
    REQUIRE(probeFor(Tap::CurveInput) == PointwiseProbe::AfterTone);
    REQUIRE_THROWS_AS(probeFor(static_cast<Tap>(7)), std::invalid_argument);
}

TEST_CASE("Sampling the curve input on the GPU matches the CPU", "[gpu][sample]") {
    GpuContext& context = gpuContext();
    const std::array<std::pair<const char*, GeometrySettings>, 3> geometries{{
        {"none", {}},
        {"straighten and crop",
         {.rotation = QuarterTurn::Clockwise90,
          .straighten = 7.5,
          .crop = {.rectangle = UprightCropRect{0.1, 0.05, 0.9, 0.8}}}},
        {"flip and ratio", {.flipHorizontal = true, .crop = {.aspect = CropRatio{3.0 / 2.0}}}},
    }};
    const std::array<RenderRequest, 3> requests{
        RenderRequest{},
        RenderRequest{.size = RenderRequest::FitInside{20, 20}},
        RenderRequest{.size = RenderRequest::FitInside{16, 16},
                      .region = RenderRequest::Region{0.2, 0.1, 0.9, 1.0},
                      .filter = ResizeFilter::Bilinear},
    };
    for (const char* name : {"linear-32x24-skewed.dng", "bayer-32x24.dng",
                             "testcard-61x41-srgb16.png", "testcard-61x41-alpha8.png"}) {
        const ImageBuffer source = fixtureImage(name);
        const DeviceImage uploaded = uploadSource(context, source);
        for (const auto& [label, geometry] : geometries) {
            for (std::size_t r = 0; r < requests.size(); ++r) {
                DYNAMIC_SECTION(name << ", " << label << ", request " << r) {
                    const DevelopState state{busy(geometry)};
                    const ImageBuffer expected =
                        sample(source, state, Tap::CurveInput, requests[r]);
                    const ImageBuffer actual =
                        sampleOnGpu(context, source, uploaded, state, Tap::CurveInput, requests[r]);
                    REQUIRE(actual.format() == workingFormat);
                    REQUIRE(actual.size() == expected.size());
                    REQUIRE(std::get<NamedEncoding>(actual.encoding()) == perceptualEncoding);

                    const double error = worstColourError(decoded(expected), decoded(actual));
                    if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
                        std::fprintf(stderr, "curve input tap: colour %.3g\n", error);
                    }
                    CAPTURE(error);
                    REQUIRE(error <= tapTolerance);

                    const CurveHistogram want = curveHistogram(expected);
                    const CurveHistogram got = curveHistogram(actual);
                    REQUIRE(got.pixels == want.pixels);
                    const auto allowed =
                        static_cast<std::uint64_t>(2.0 * binTolerance * want.pixels) + 2;
                    for (const auto channel : {&CurveHistogram::luma, &CurveHistogram::red,
                                               &CurveHistogram::green, &CurveHistogram::blue}) {
                        const std::uint64_t distance = binDistance(want.*channel, got.*channel);
                        if (std::getenv("ARRAW_PRINT_MEASURED") != nullptr) {
                            std::fprintf(stderr, "curve histogram: %llu of %llu counts moved\n",
                                         static_cast<unsigned long long>(distance),
                                         static_cast<unsigned long long>(want.pixels));
                        }
                        CAPTURE(distance, allowed);
                        REQUIRE(distance <= allowed);
                    }
                }
            }
        }
    }
}

TEST_CASE("Sampling from the host equals sampling from an uploaded source", "[gpu][sample]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = fixtureImage("linear-32x24-rotated.dng");
    const DevelopState state{busy({.straighten = 4.0})};
    const RenderRequest request{.size = RenderRequest::FitInside{12, 12}};
    const ImageBuffer fromHost = sampleOnGpu(context, source, state, Tap::CurveInput, request);
    const ImageBuffer fromDevice = sampleOnGpu(context, source, uploadSource(context, source),
                                               state, Tap::CurveInput, request);
    REQUIRE(compareFloat(fromHost, fromDevice).bitExact);
}

TEST_CASE("Sampling on the GPU ignores the curves and refuses what it cannot do", "[gpu][sample]") {
    GpuContext& context = gpuContext();
    const ImageBuffer source = fixtureImage("testcard-61x41-srgb8.png");
    DevelopSettings plain;
    plain.tone.exposure = 0.3F;
    DevelopSettings curved = plain;
    curved.toneCurve.luma.points = {{0.0F, 0.3F}, {1.0F, 0.6F}};
    curved.tone.filmicHighlights = 90.0F;
    const ImageBuffer first = sampleOnGpu(context, source, DevelopState{plain}, Tap::CurveInput);
    const ImageBuffer second = sampleOnGpu(context, source, DevelopState{curved}, Tap::CurveInput);
    REQUIRE(compareFloat(first, second).bitExact);

    REQUIRE_THROWS_AS(sampleOnGpu(context, source, {}, static_cast<Tap>(7)), std::invalid_argument);
    const ImageBuffer other = fixtureImage("linear-32x24-neutral.dng");
    REQUIRE_THROWS_AS(
        sampleOnGpu(context, source, uploadSource(context, other), {}, Tap::CurveInput),
        std::invalid_argument);
}
