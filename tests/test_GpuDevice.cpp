#include "GpuDevice.h"

#include <rhi/qrhi.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>

using namespace arraw;
using Catch::Matchers::Equals;

/// Tests for the transfer helper every upload and readback goes through, on
/// QRhi's Null backend: it keeps frames and batches as a real device does, and
/// needs no GPU and no QGuiApplication.

namespace {

/// @brief Creates a device on QRhi's Null backend.
detail::GpuDevice nullDevice() {
    detail::GpuDevice device;
    QRhiNullInitParams params;
    device.rhi.reset(QRhi::create(QRhi::Null, &params));
    return device;
}

} // namespace

TEST_CASE("A recording that throws leaves the device able to submit again", "[gpu]") {
    detail::GpuDevice device = nullDevice();
    REQUIRE(device.rhi != nullptr);

    /// More failures than QRhi has batches, so a batch that is never released
    /// runs the pool dry, and a frame that is never ended refuses the next one.
    constexpr int failures = 100;
    for (int attempt = 0; attempt < failures; ++attempt) {
        CAPTURE(attempt);
        REQUIRE_THROWS_WITH(detail::submitUpdates(device, "test",
                                                  [](QRhiResourceUpdateBatch&) {
                                                      throw std::runtime_error("recording failed");
                                                  }),
                            Equals("recording failed"));
    }

    REQUIRE_NOTHROW(detail::submitUpdates(device, "test", [](QRhiResourceUpdateBatch&) {}));
    REQUIRE_FALSE(device.lost);
}
