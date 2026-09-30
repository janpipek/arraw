#include "GpuContext.h"
#include "GpuTesting.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace arraw;
using namespace arraw::test;

// A backend need not list adapters (Qt's OpenGL does not): then adapter 0 means
// its default device, and the cases below expect that instead.

TEST_CASE("The backend lists adapters that have names", "[gpu][adapters]") {
    CAPTURE(sharedGpuContextStatus());
    (void)gpuContext(); // Skips where this machine has no device for the backend.

    for (const GpuAdapterInfo& adapter : listGpuAdapters(gpuTestBackend())) {
        CAPTURE(adapter.name);
        REQUIRE_FALSE(adapter.name.empty());
    }
}

TEST_CASE("A context on an adapter is that adapter", "[gpu][adapters]") {
    (void)gpuContext();
    const std::vector<GpuAdapterInfo> adapters = listGpuAdapters(gpuTestBackend());

    const GpuContext context(gpuTestBackend(), 0);
    REQUIRE(context.info().adapter == std::optional<std::size_t>{0});
    if (!adapters.empty()) {
        REQUIRE(context.info().deviceName == adapters[0].name);
        REQUIRE(context.info().kind == adapters[0].kind);
        REQUIRE(context.info().vendorId == adapters[0].vendorId);
        REQUIRE(context.info().deviceId == adapters[0].deviceId);
    }

    /// The default device names no adapter.
    const GpuContext defaulted(gpuTestBackend());
    REQUIRE_FALSE(defaulted.info().adapter.has_value());
}

TEST_CASE("An adapter past the end is refused, with the count", "[gpu][adapters]") {
    (void)gpuContext();
    const std::size_t count = listGpuAdapters(gpuTestBackend()).size();
    // With no adapters listed, 0 is the default device and 1 is the first past the end.
    const std::size_t past = std::max<std::size_t>(count, 1);
    const std::string expected =
        count == 0 ? "does not list adapters" : "lists " + std::to_string(count);

    try {
        const GpuContext context(gpuTestBackend(), past);
        FAIL("a context was made on an adapter that does not exist");
    } catch (const std::out_of_range& error) {
        REQUIRE(std::string(error.what()).find(expected) != std::string::npos);
    }
}
