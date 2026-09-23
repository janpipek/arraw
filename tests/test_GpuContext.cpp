#include <GpuContext.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <set>
#include <stdexcept>
#include <string_view>

using namespace arraw;

/// Tests for what the GPU module promises without a GPU. The suite runs inside
/// a QCoreApplication and no runner is assumed to have a device, so nothing
/// here creates one; the round trip itself is `arraw-cli gpu-test`'s to judge,
/// on a machine that has the hardware.

TEST_CASE("Backend names round-trip and nothing else parses", "[gpu]") {
    const auto backend = GENERATE(GpuBackend::Vulkan, GpuBackend::OpenGL, GpuBackend::D3D11,
                                  GpuBackend::D3D12, GpuBackend::Metal);
    CAPTURE(gpuBackendName(backend));
    REQUIRE(parseGpuBackend(gpuBackendName(backend)) == backend);

    /// Exact names only: the command line lower-cases what it was given, and
    /// nothing guesses at what a near miss meant.
    for (const std::string_view name :
         {"", "Vulkan", "gl", "gles2", "d3d", "dx12", "metal ", "null"}) {
        CAPTURE(name);
        REQUIRE_FALSE(parseGpuBackend(name).has_value());
    }
}

TEST_CASE("Every backend has its own name", "[gpu]") {
    const std::set<std::string_view> names{
        gpuBackendName(GpuBackend::Vulkan), gpuBackendName(GpuBackend::OpenGL),
        gpuBackendName(GpuBackend::D3D11), gpuBackendName(GpuBackend::D3D12),
        gpuBackendName(GpuBackend::Metal)};
    REQUIRE(names.size() == 5);
}

TEST_CASE("Each platform defaults to its own backend", "[gpu]") {
#if defined(_WIN32)
    REQUIRE(defaultGpuBackend() == GpuBackend::D3D11);
#elif defined(__APPLE__)
    REQUIRE(defaultGpuBackend() == GpuBackend::Metal);
#else
    REQUIRE(defaultGpuBackend() == GpuBackend::Vulkan);
#endif
}

TEST_CASE("A device is refused without a GUI application, never faked", "[gpu]") {
    const auto backend = GENERATE(GpuBackend::Vulkan, GpuBackend::OpenGL, GpuBackend::D3D11,
                                  GpuBackend::D3D12, GpuBackend::Metal);
    CAPTURE(gpuBackendName(backend));

    /// The suite's QCoreApplication has no platform plugin to make a device
    /// through. The answer is an error for every backend, rather than a device
    /// that is secretly something else (ADR 015).
    REQUIRE_THROWS_AS(GpuContext{backend}, std::runtime_error);
}
