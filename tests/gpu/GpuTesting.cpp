#include "GpuTesting.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>

namespace arraw::test {

namespace {

/// @brief The shared device; null when creation failed.
std::unique_ptr<GpuContext> shared;

/// @brief Device name, or the creation error.
std::string status = "No GPU context was created";

/// @brief Measures two samples' distance; identical bits and two NaNs agree, one NaN is
/// infinitely far.
double sampleDifference(float expected, float actual) {
    if (std::bit_cast<std::uint32_t>(expected) == std::bit_cast<std::uint32_t>(actual)) {
        return 0.0;
    }
    if (std::isnan(expected) && std::isnan(actual)) {
        return 0.0;
    }
    if (std::isnan(expected) || std::isnan(actual)) {
        return std::numeric_limits<double>::infinity();
    }
    return std::abs(static_cast<double>(expected) - static_cast<double>(actual));
}

} // namespace

void createSharedGpuContext() {
    try {
        shared = std::make_unique<GpuContext>(GpuBackend::Vulkan);
        status = shared->info().deviceName;
    } catch (const std::exception& problem) {
        shared.reset();
        status = problem.what();
    }
}

void destroySharedGpuContext() {
    shared.reset();
}

const std::string& sharedGpuContextStatus() {
    return status;
}

GpuContext& gpuContext() {
    if (!shared) {
        SKIP("No Vulkan device on this machine: " << status);
    }
    return *shared;
}

FloatDifference compareFloat(const ImageBuffer& expected, const ImageBuffer& actual,
                             double absoluteFloor) {
    if (expected.format() != PixelFormat::RgbaF32 || actual.format() != PixelFormat::RgbaF32 ||
        expected.size() != actual.size()) {
        throw std::invalid_argument("compareFloat needs two RgbaF32 images of one size");
    }
    const std::span<const float> want = expected.samples<float>();
    const std::span<const float> got = actual.samples<float>();
    constexpr std::size_t channels = 4;
    const auto width = static_cast<std::size_t>(expected.size().width);

    FloatDifference result;
    double worstRelative = -1.0;
    for (std::size_t index = 0; index < want.size(); ++index) {
        if (std::bit_cast<std::uint32_t>(want[index]) != std::bit_cast<std::uint32_t>(got[index])) {
            result.bitExact = false;
        }
        const double difference = sampleDifference(want[index], got[index]);
        const double relative =
            difference / std::max(std::abs(static_cast<double>(want[index])), absoluteFloor);
        result.maxAbsDiff = std::max(result.maxAbsDiff, difference);
        if (relative > worstRelative) {
            worstRelative = relative;
            result.maxRelDiff = relative;
            const std::size_t pixel = index / channels;
            result.worstX = static_cast<int>(pixel % width);
            result.worstY = static_cast<int>(pixel / width);
            result.worstChannel = static_cast<int>(index % channels);
            result.worstExpected = want[index];
            result.worstActual = got[index];
        }
    }
    return result;
}

std::ostream& operator<<(std::ostream& stream, const FloatDifference& difference) {
    return stream << "max abs " << difference.maxAbsDiff << ", max rel " << difference.maxRelDiff
                  << (difference.bitExact ? ", bit exact" : ", not bit exact") << "; worst at ("
                  << difference.worstX << ", " << difference.worstY << ") channel "
                  << difference.worstChannel << ": expected " << difference.worstExpected
                  << ", got " << difference.worstActual;
}

} // namespace arraw::test
