#pragma once

#include "GpuContext.h"

#include <ImageBuffer.h>

#include <cstddef>
#include <iosfwd>
#include <string>

/// Helpers for tests that run passes on a real device (see main.cpp).

namespace arraw::test {

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
/// creation error when this machine has no Vulkan device.
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

} // namespace arraw::test
