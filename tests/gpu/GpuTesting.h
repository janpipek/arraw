#pragma once

#include "GpuContext.h"

#include <ImageBuffer.h>

#include <cstddef>
#include <iosfwd>
#include <string>

/// Helpers for tests that run passes on a real device (see main.cpp).

namespace arraw::test {

/// @brief Names the backend every GPU test runs on.
///
/// Read once from ARRAW_TEST_GPU_BACKEND (the names ::arraw::parseGpuBackend
/// reads), defaulting to ::arraw::defaultGpuBackend(). Tests use this, never a
/// literal backend, so that one suite covers Vulkan, OpenGL, Direct3D and Metal.
/// @return The backend under test.
/// @throws std::runtime_error if the variable names no backend; main() calls it
/// before anything else, so a misspelt name fails the run rather than skipping it.
[[nodiscard]] GpuBackend gpuTestBackend();

/// @brief Creates the device every GPU test shares, or records why it cannot.
///
/// Called once by main(), after the application exists; never throws.
void createSharedGpuContext();

/// @brief Releases the shared device; main() calls it before the application ends.
void destroySharedGpuContext();

/// @brief Names the shared device, or the reason there is none.
[[nodiscard]] const std::string& sharedGpuContextStatus();

/// @brief Hands out the shared device, or skips the calling test.
///
/// Skips through Catch (`SKIP`, exit code 4 if nothing else ran) with the
/// creation error, naming the backend, when this machine has no device for it.
/// @return The shared context; valid until the process ends.
[[nodiscard]] GpuContext& gpuContext();

/// @brief How far apart two RGBA float images are, sample by sample.
struct FloatDifference {
    double maxAbsDiff = 0.0;    ///< Largest absolute difference of any sample.
    double maxRelDiff = 0.0;    ///< Largest difference relative to max(|expected|, floor).
    int worstX = 0;             ///< Column of the sample with the largest relative difference.
    int worstY = 0;             ///< Row of that sample.
    int worstChannel = 0;       ///< Channel of that sample: 0 red to 3 alpha.
    float worstExpected = 0.0F; ///< Expected value at that sample.
    float worstActual = 0.0F;   ///< Actual value at that sample.
    bool bitExact = true;       ///< Whether every sample has the same bits.

    friend std::ostream& operator<<(std::ostream& stream, const FloatDifference& difference);
};

/// @brief Compares two RGBA float images.
/// @param expected Reference image.
/// @param actual Image under test; the same size and ::arraw::PixelFormat::RgbaF32.
/// @param absoluteFloor Magnitude below which a difference counts against the
/// floor rather than the expected value, so that values near zero do not blow
/// up the relative error.
/// @return The differences. Encoding and orientation are not compared.
/// @throws std::invalid_argument if the images differ in size or are not RgbaF32.
[[nodiscard]] FloatDifference compareFloat(const ImageBuffer& expected, const ImageBuffer& actual,
                                           double absoluteFloor = 1e-6);

// Tolerances against the CPU, measured on lavapipe by the steps that fixed them.
//
// Every number below, and the NaN and infinity parity the tests assert, was
// measured on Vulkan (lavapipe, Mesa llvmpipe) only, although the suite can now
// run on any backend (see gpuTestBackend). GLSL for OpenGL, HLSL and MSL back
// ends may compile with fast-math, which changes comparisons against NaN and
// infinity and how `pow` rounds; none of that is verified here.

/// @brief Largest error the pointwise pass may have against the CPU chain, relative to the
/// pixel's scale (see worstColourError).
///
/// Measured worst case on lavapipe (Mesa llvmpipe), over every case below:
/// 1.7e-5 with the floor below, 4.4e-5 with a floor of 1e-6. The chain raises
/// to a power twice, and GLSL leaves `pow` precision to the implementation
/// (Vulkan allows several ULP; lavapipe's is a polynomial exp2/log2), against
/// the CPU's correctly rounded `std::pow`. The worst cases are the ones where
/// the chain subtracts nearly equal perceptual values (Blacks at -100 pulls a
/// dark value down to just above zero before it is raised back to 2.2), which
/// turns a power's relative error into a larger one in the result. The rest is
/// plain float arithmetic that a GPU may fuse into multiply-adds.
///
/// Looser than the plan's 1e-5 target, and the floor higher than its 1e-6, for
/// that reason; a wrong stage, matrix layout or order disagrees by 1e-3 or more.
inline constexpr double pointwiseRelativeTolerance = 3e-5;

/// @brief Largest error the pointwise pass may have against the CPU chain on ill-conditioned
/// input, relative to the pixel's scale (see worstColourError).
///
/// Ill-conditioned: negative channels, or a luminance near zero under channels
/// far from it (`{-1, 0, 4.43}` develops to -410 and 1840). There the chain's
/// three powers and a subtraction of nearly equal perceptual values magnify
/// `pow`'s rounding, and how much depends on the driver. Measured worst case:
/// within pointwiseRelativeTolerance on lavapipe, 3.6e-4 on Mesa ANV (Intel HD
/// Graphics 630), whose `pow` is within Vulkan's precision but less exact than
/// lavapipe's. Well-conditioned input holds pointwiseRelativeTolerance on both.
///
/// The cases held to this bound check which branch the chain takes (lifted
/// black, negatives through the tone curve), and a wrong branch disagrees by
/// 1e-2 or more.
inline constexpr double illConditionedRelativeTolerance = 5e-4;

/// @brief Magnitude below which the pointwise comparison uses an absolute error instead.
inline constexpr double pointwiseAbsoluteFloor = 1e-5;

/// Largest relative difference tolerated where the resample blends texels.
///
/// Measured worst on small images (up to 31x20, all angles, crops and
/// aspects): 1.6e-6, and 2.2e-6 on a 6000 pixel wide checker. Kept at 1e-4, a
/// wide margin, so that end-to-end comparisons that add the pointwise error do
/// not become fragile; the geometry tests assert tighter bounds of their own
/// where they have measured them. The shader carries the source position as a
/// whole and a fractional part and evaluates the blend weights from the latter,
/// so the error is a few float ulps of the weight, whatever the source size.
inline constexpr double resampleTolerance = 1.0e-4;

/// Magnitude below which differences count against a floor, not the expected value.
inline constexpr double geometryAbsoluteFloor = 1.0e-4;

/// @brief Largest absolute difference tolerated in a resize, in colour of well-conditioned
/// windows and in alpha everywhere.
///
/// Measured worst on lavapipe over every case in test_GpuResize.cpp (Lanczos and
/// bilinear; 8x, 13.7x and non-integer shrinks, boxes down to 1x1, enlargements
/// to 2.5x, rotated and cropped geometry in front): 7.8e-7 in colour and 6.6e-7
/// in alpha, a few float ulps of values up to 3.5. The weights are the CPU's,
/// computed in double and uploaded, so the difference is the sum done in float
/// (up to 83 taps at 13.7x) against the CPU's double, and a GPU's fused
/// multiply-adds. A wrong tap, centre or edge rule disagrees by 1e-3 or more.
/// The opaque passes (one plane, no tracking) agree bit for bit with the general
/// ones on opaque images on lavapipe, and are held to the same tolerance.
///
/// Well-conditioned: opaque, or a window whose colour is a plain weighted mean.
inline constexpr double resizeTolerance = 1.0e-5;

/// @brief Largest absolute colour difference tolerated in a resize where the result is a
/// quotient or a difference of nearly equal sums.
///
/// Ill-conditioned: pixels whose window holds transparency (the colour is
/// premultiplied colour over alpha, so it divides the float sums' error by
/// alpha, here taken at or above resizeVisibleAlpha), and images with negative
/// channels, where Lanczos lobes of opposite sign cancel. Measured worst on
/// lavapipe: 7.2e-7 beside transparency (opaque pixels after a rotation
/// included, see geometry.frag on why those must come out exactly opaque) and
/// 1.7e-6 with negative and above-one channels. The bound is looser than the
/// measurement by the factor the pointwise bounds are, because how much a
/// division or a cancellation costs depends on the driver's arithmetic, and
/// only lavapipe was measured.
inline constexpr double illConditionedResizeTolerance = 1.0e-4;

/// @brief Output alpha below which a resize's colour is not compared.
///
/// Colour is a quotient by alpha, so close to transparent a float sum's
/// rounding is magnified without bound (and it is clamped to the window's
/// visible range there, which hides nothing); alpha itself is still compared.
inline constexpr float resizeVisibleAlpha = 5.0e-2F;

/// @brief Measures the worst colour error of an image, relative to each pixel's own scale.
///
/// See the implementation for the metric; alpha is not part of it.
/// @param expected Reference image; RgbaF32.
/// @param actual Image under test, the same size and format.
/// @return The largest error, with non-finite disagreements counted as infinite.
[[nodiscard]] double worstColourError(const ImageBuffer& expected, const ImageBuffer& actual);

} // namespace arraw::test
