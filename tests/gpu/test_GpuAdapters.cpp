#include "GpuContext.h"
#include "GpuTesting.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::test;

TEST_CASE("Vulkan lists at least one adapter", "[gpu][adapters]") {
    CAPTURE(sharedGpuContextStatus());
    (void)gpuContext(); // Skips where this machine has no Vulkan device.

    const std::vector<GpuAdapterInfo> adapters = listGpuAdapters(GpuBackend::Vulkan);
    REQUIRE_FALSE(adapters.empty());
    for (const GpuAdapterInfo& adapter : adapters) {
        CAPTURE(adapter.name);
        REQUIRE_FALSE(adapter.name.empty());
    }
}

TEST_CASE("A context on an adapter is that adapter", "[gpu][adapters]") {
    (void)gpuContext();
    const std::vector<GpuAdapterInfo> adapters = listGpuAdapters(GpuBackend::Vulkan);
    REQUIRE_FALSE(adapters.empty());

    const GpuContext context(GpuBackend::Vulkan, 0);
    REQUIRE(context.info().adapter == std::optional<std::size_t>{0});
    REQUIRE(context.info().deviceName == adapters[0].name);
    REQUIRE(context.info().kind == adapters[0].kind);
    REQUIRE(context.info().vendorId == adapters[0].vendorId);
    REQUIRE(context.info().deviceId == adapters[0].deviceId);

    /// The default device names no adapter.
    const GpuContext defaulted(GpuBackend::Vulkan);
    REQUIRE_FALSE(defaulted.info().adapter.has_value());
}

TEST_CASE("An adapter past the end is refused, with the count", "[gpu][adapters]") {
    (void)gpuContext();
    const std::size_t count = listGpuAdapters(GpuBackend::Vulkan).size();
    REQUIRE(count > 0);

    try {
        const GpuContext context(GpuBackend::Vulkan, count);
        FAIL("a context was made on an adapter that does not exist");
    } catch (const std::out_of_range& error) {
        REQUIRE(std::string(error.what()).find("lists " + std::to_string(count)) !=
                std::string::npos);
    }
}
