#pragma once

#include "DeviceImage.h"

#include <ImageBuffer.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace arraw {

namespace detail {
struct GpuDevice;
} // namespace detail

/// @brief Graphics API a ::arraw::GpuContext drives, through QRhi.
///
/// Every backend QRhi offers bar its null one, which renders nothing and so
/// could only ever pass a probe by lying. Which of them exist in a given build
/// is Qt's and the platform's business; an unavailable one is refused when a
/// context is made, never replaced by another.
enum class GpuBackend {
    Vulkan,
    OpenGL,
    D3D11,
    D3D12,
    Metal,
};

/// @brief Chooses the backend this platform is best served by.
///
/// Vulkan on Linux, Direct3D 11 on Windows (QRhi's most mature backend there;
/// Direct3D 12 remains selectable), Metal on macOS.
[[nodiscard]] GpuBackend defaultGpuBackend() noexcept;

/// @brief Names a backend as the command line spells it.
/// @return `vulkan`, `opengl`, `d3d11`, `d3d12` or `metal`.
[[nodiscard]] std::string_view gpuBackendName(GpuBackend backend) noexcept;

/// @brief Reads a backend from the name ::arraw::gpuBackendName gives it.
/// @param name Exact, lower-case name.
/// @return The backend, or `std::nullopt` if no backend is called that.
[[nodiscard]] std::optional<GpuBackend> parseGpuBackend(std::string_view name) noexcept;

/// @brief What sort of hardware, if any, stands behind a device.
///
/// Mirrors what QRhi reports, with one addition: backends that never say
/// (OpenGL reports nothing) are classified by device name when that name is a
/// known software rasteriser, because a CPU pretending to be a GPU is exactly
/// what a probe must not wave through.
enum class GpuDeviceKind {
    Unknown,
    Integrated,
    Discrete,
    External,
    Virtual,
    Software, ///< A CPU rasteriser: WARP, llvmpipe, lavapipe, SwiftShader.
};

/// @brief Everything a caller may want to know about a device before using it.
struct GpuDeviceInfo {
    /// @brief Backend the device was created through.
    GpuBackend backend = GpuBackend::Vulkan;

    /// @brief Name the driver gives the device, UTF-8.
    std::string deviceName;

    /// @brief Sort of hardware behind the device.
    GpuDeviceKind kind = GpuDeviceKind::Unknown;

    /// @brief PCI vendor id, or 0 where the backend does not report one.
    std::uint64_t vendorId = 0;

    /// @brief PCI device id, or 0 where the backend does not report one.
    std::uint64_t deviceId = 0;

    /// @brief Whether RGBA32F textures are supported: the working format's own.
    bool floatTextures = false;

    /// @brief Whether RGBA16F textures are supported: the precision trade-off
    /// ADR 015 leaves open.
    bool halfFloatTextures = false;

    /// @brief Whether compute shaders are supported.
    bool compute = false;

    /// @brief Whether the backend promises to read back textures of any format.
    ///
    /// QRhi's OpenGL backend does not, so a float round trip there is a
    /// driver's courtesy rather than a contract; the probe is how to find out.
    bool anyFormatReadBack = false;

    /// @brief Largest texture edge the device accepts, in pixels.
    int maxTextureSize = 0;
};

/// @brief One fullscreen fragment pass a ::arraw::GpuContext can render.
///
/// Each reads one input image through `texelFetch` at binding 0 and, except
/// ::Copy, one std140 uniform block at binding 1, and writes one RGBA32F
/// render target. The shaders live in `src/gpu/shaders`, compiled at build time.
enum class GpuPass {
    Copy,      ///< Copies its input unchanged; no uniforms. The render round trip's proof.
    Pointwise, ///< The pointwise chain; uniforms are a ::arraw::GpuPointwiseBlock.
    Geometry,  ///< The geometry resample; uniforms are a ::arraw::GpuGeometryBlock.
};

/// @brief Number of ::arraw::GpuPass values, for tables indexed by one.
inline constexpr std::size_t gpuPassCount = 3;

/// @brief One graphics device, owned, offscreen, on the thread that made it.
///
/// The adapter ADR 015 describes: pass-recording code will take a device it
/// does not own, and this is the thing that owns one for callers with no
/// viewport — the command line, export, and the CPU/GPU comparison tests.
///
/// Never falls back. A backend that is not in this Qt, not on this platform or
/// will not start is an error, and so the caller learns that it has no GPU
/// rather than comparing the CPU with itself.
///
/// QRhi is used from one thread, so this is too: the thread that constructs a
/// context is its owner, and uploading to or reading back from it anywhere else
/// throws rather than corrupting the device. Needs a `QGuiApplication`, since
/// Vulkan instances and OpenGL surfaces come from the platform plugin.
///
/// A transfer whose frame fails leaves the device unusable: every later
/// transfer, through the context or its images, throws. A new context is the
/// way back.
///
/// Neither copyable nor movable. Images it mints share the device, so they may
/// outlive the context; they still belong to its owner thread, and the context
/// and every image's last copy must be destroyed there too, since that is where
/// the texture, and perhaps the device itself, is released. Debug builds assert
/// it. Handing an image to another thread and back is fine; dropping it there
/// is not.
///
/// On Metal, an owner thread other than the main one must keep an autorelease
/// pool around its use of the context and its images, as QRhi requires of a
/// dedicated render thread; nothing here makes one.
class GpuContext {
public:
    /// @brief Creates an offscreen device through one backend.
    /// @param backend Backend to create; never substituted.
    /// @throws std::runtime_error if no `QGuiApplication` exists, the backend is
    /// not available in this build or on this platform, or the device cannot be
    /// created.
    explicit GpuContext(GpuBackend backend);

    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;
    GpuContext(GpuContext&&) = delete;
    GpuContext& operator=(GpuContext&&) = delete;
    ~GpuContext();

    /// @brief Identity the images this context mints carry.
    [[nodiscard]] DeviceId id() const noexcept;

    /// @brief Description of the device and what it supports.
    [[nodiscard]] const GpuDeviceInfo& info() const noexcept;

    /// @brief Copies a host buffer into a new RGBA32F texture.
    /// @param image Buffer to upload; must be ::arraw::PixelFormat::RgbaF32.
    /// @return A device image with the buffer's size, encoding and orientation.
    /// @throws std::invalid_argument if @p image is not RGBA float, or is larger
    /// than the device or a single QRhi transfer accepts.
    /// @throws std::logic_error if called from a thread other than the owner.
    /// @throws std::runtime_error if the device has no RGBA32F textures (see
    /// GpuDeviceInfo::floatTextures), cannot create or fill the texture, or an
    /// earlier transfer failed.
    [[nodiscard]] DeviceImage upload(const ImageBuffer& image);

    /// @brief Renders one pass from a device image into a new one.
    ///
    /// The one way a pass's output becomes a ::arraw::DeviceImage, which is
    /// what keeps this class the only minter of them. Pipelines are built on a
    /// pass's first use and kept for the device's lifetime. Waits for the
    /// render to finish, as a transfer does.
    /// @param pass Shader to run.
    /// @param uniforms The pass's uniform block, byte for byte; empty for ::GpuPass::Copy.
    /// @param input Image the pass reads; must belong to this context's device.
    /// @param outputSize Dimensions of the result.
    /// @param encoding Meaning of the result's RGB values, which the pass decides.
    /// @return The result, with no pending orientation.
    /// @throws std::invalid_argument if @p input is empty or belongs to another
    /// device, @p uniforms is not the pass's block size, or @p outputSize is
    /// empty or larger than the device accepts.
    /// @throws std::logic_error if called from a thread other than the owner.
    /// @throws std::runtime_error if the device has no RGBA32F textures, cannot
    /// create the pass's resources, or the render fails.
    [[nodiscard]] DeviceImage render(GpuPass pass, std::span<const std::byte> uniforms,
                                     const DeviceImage& input, ImageSize outputSize,
                                     const ColorEncoding& encoding);

private:
    /// @brief Device shared with every image minted from it.
    std::shared_ptr<detail::GpuDevice> device_;

    /// @brief Description gathered once, when the device was created.
    GpuDeviceInfo info_;
};

} // namespace arraw
