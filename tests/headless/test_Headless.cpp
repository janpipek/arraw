#include <GpuContext.h>
#include <HeadlessPlatform.h>

#include <ColorEncoding.h>
#include <ImageBuffer.h>
#include <ImageOrientation.h>

#include <QGuiApplication>
#include <QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

using namespace arraw;
using Catch::Matchers::ContainsSubstring;

/// Tests for arraw's headless Qt platform, run on it (see main.cpp). What it
/// promises is a Vulkan device without a display, so that is what is tried; a
/// runner with no Vulkan driver at all skips those cases rather than failing,
/// and a software one (lavapipe, on CI) is as good as a GPU for them, since
/// the platform is under test rather than the hardware.

namespace {

/// @brief Creates a Vulkan context, or says why there is none.
/// @param reason Set to the error when creation fails.
/// @return The context, or null if this machine cannot make one.
std::unique_ptr<GpuContext> vulkanContext(std::string& reason) {
    try {
        return std::make_unique<GpuContext>(GpuBackend::Vulkan);
    } catch (const std::exception& problem) {
        reason = problem.what();
        return nullptr;
    }
}

} // namespace

TEST_CASE("The application runs on arraw's headless platform", "[headless]") {
    REQUIRE(QGuiApplication::platformName().toStdString() == headless::platformKey);
    /// One screen, so that code asking what a screen is like gets an answer.
    REQUIRE(QGuiApplication::primaryScreen() != nullptr);
}

TEST_CASE("A Vulkan device can be made without a display", "[headless][gpu]") {
    std::string reason;
    const auto context = vulkanContext(reason);
    if (!context) {
        SKIP("No Vulkan device on this machine: " << reason);
    }

    const GpuDeviceInfo& info = context->info();
    CAPTURE(info.deviceName);
    REQUIRE(info.backend == GpuBackend::Vulkan);
    REQUIRE(context->id() != DeviceId::None);
    REQUIRE(info.maxTextureSize > 0);
}

TEST_CASE("An RGBA float image survives a Vulkan round trip bit for bit", "[headless][gpu]") {
    std::string reason;
    const auto context = vulkanContext(reason);
    if (!context) {
        SKIP("No Vulkan device on this machine: " << reason);
    }
    CAPTURE(context->info().deviceName);
    REQUIRE(context->info().floatTextures);

    /// What the CPU chain deliberately keeps and a lesser format or converting
    /// copy would not: negatives, values above white, signed zero, subnormals,
    /// the half-float boundaries and fractional alpha.
    constexpr std::array colour{
        0.0F,
        -0.0F,
        1.0F,
        -1.0F,
        -0.25F,
        2.0F,
        16.0F,
        65504.0F,
        65520.0F,
        1.0e6F,
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::min(),
        1.0e-40F,
        std::numeric_limits<float>::denorm_min(),
        -std::numeric_limits<float>::denorm_min(),
        1.0e-8F,
    };
    constexpr std::array alpha{0.0F, 1.0F, 0.5F, 1.0F / 3.0F, 1.0F / 255.0F, 0.999999F};

    /// Not upright, so that the description surviving the trip is checked too.
    ImageBuffer sent({8, 8}, PixelFormat::RgbaF32, workingEncoding, ImageOrientation::Rotate90);
    const std::span<float> samples = sent.samples<float>();
    constexpr std::size_t channels = 4;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const std::size_t pixel = index / channels;
        samples[index] = index % channels == channels - 1 ? alpha[pixel % alpha.size()]
                                                          : colour[index % colour.size()];
    }

    const DeviceImage uploaded = context->upload(sent);
    REQUIRE(uploaded.device() == context->id());
    const ImageBuffer received = uploaded.readBack();

    REQUIRE(received.size() == sent.size());
    REQUIRE(received.format() == sent.format());
    REQUIRE(received.encoding() == sent.encoding());
    REQUIRE(received.orientation() == sent.orientation());
    const std::span<const float> back = received.samples<float>();
    REQUIRE(back.size() == samples.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        /// By bits: `==` would call -0 and 0 equal.
        CAPTURE(index, samples[index], back[index]);
        REQUIRE(std::bit_cast<std::uint32_t>(back[index]) ==
                std::bit_cast<std::uint32_t>(samples[index]));
    }
}

TEST_CASE("OpenGL is refused on the headless platform, pointing at one that has it",
          "[headless][gpu]") {
    /// The platform makes no OpenGL contexts. The answer is an error that says
    /// how to reach OpenGL, before Qt tries and warns, never another backend.
#if QT_CONFIG(opengl)
    REQUIRE_THROWS_WITH(GpuContext{GpuBackend::OpenGL}, ContainsSubstring("QT_QPA_PLATFORM"));
#endif
    REQUIRE_THROWS_AS(GpuContext{GpuBackend::OpenGL}, std::runtime_error);
}
