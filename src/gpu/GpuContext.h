#pragma once

#include "DeviceImage.h"
#include "PixelRect.h"

#include <ImageBuffer.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// @brief Tells whether an automatic choice of device takes the GPU on this platform.
///
/// False on Windows for now: Direct3D 11, its default backend, has not run the
/// CPU parity suite, so `auto` develops on the CPU there until it has (ADR 017).
/// An explicit choice of the GPU (`--device gpu`, or an adapter named in the
/// app's settings) still takes it.
[[nodiscard]] bool gpuUsedByDefault() noexcept;

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

/// @brief What a backend says about one of its adapters, before a device is made from it.
///
/// Classified as ::arraw::GpuDeviceInfo is, so that a software rasteriser is
/// recognised here too, and a caller can refuse it without creating a device.
struct GpuAdapterInfo {
    /// @brief Name the driver gives the adapter, UTF-8.
    std::string name;

    /// @brief Sort of hardware behind the adapter.
    GpuDeviceKind kind = GpuDeviceKind::Unknown;

    /// @brief PCI vendor id, or 0 where the backend does not report one.
    std::uint64_t vendorId = 0;

    /// @brief PCI device id, or 0 where the backend does not report one.
    std::uint64_t deviceId = 0;
};

/// @brief Lists the adapters a backend enumerates, in the order its indices number them.
///
/// The position in the result is the index ::arraw::GpuContext takes. Empty
/// where the backend does not enumerate (OpenGL; possibly Metal), which is not
/// the same as having no device: index 0 still means the default one there.
/// Creates and drops a Vulkan instance or an OpenGL surface, as a context would.
/// @param backend Backend to ask; never substituted.
/// @throws std::runtime_error if no `QGuiApplication` exists, or the backend is
/// not available in this build or on this platform.
[[nodiscard]] std::vector<GpuAdapterInfo> listGpuAdapters(GpuBackend backend);

/// @brief Everything a caller may want to know about a device before using it.
struct GpuDeviceInfo {
    /// @brief Backend the device was created through.
    GpuBackend backend = GpuBackend::Vulkan;

    /// @brief Index of the adapter the device was made on, if one was asked for.
    ///
    /// The position in ::arraw::listGpuAdapters. Empty for the backend's
    /// default device; also 0, not empty, when 0 was asked for of a backend
    /// that lists nothing.
    std::optional<std::size_t> adapter;

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

    /// @brief Whether R32F textures are supported, which scalar render targets use.
    ///
    /// Without them a ::arraw::GpuTargetFormat::R32F target is made RGBA32F,
    /// which holds the same value in its first channel at four times the memory.
    bool scalarFloatTextures = false;

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
/// Each reads its input images through `texelFetch`, the first at binding 0 and
/// any others from binding 2 on, and, except ::Copy, one std140 uniform block at
/// binding 1, and writes one render target, RGBA32F unless the render asks for
/// another ::arraw::GpuTargetFormat. The shaders live in
/// `src/gpu/shaders`, compiled at build time.
enum class GpuPass {
    Copy, ///< Copies its input unchanged; no uniforms. The render round trip's proof.
    /// @brief The pointwise chain; uniforms are a ::arraw::GpuPointwiseBlock.
    ///
    /// Inputs are the image, the curves, the Presence context's fine base,
    /// coarse base, coarse cells, haze floor and haze mean, and the four brush coverage textures
    /// (bindings 0 and 2 to 11); the image stands in for any the block says are not read.
    Pointwise,
    Geometry, ///< The geometry resample; uniforms are a ::arraw::GpuGeometryBlock.

    /// @brief The horizontal half of a resize, one ::arraw::ResizePlane per render.
    ///
    /// Inputs are the image and the weights of ::arraw::packResizeWeights, as
    /// uploaded; uniforms are a ::arraw::GpuResizeBlock.
    ResizeAcross,

    /// @brief The vertical half of a resize, which also produces the result.
    ///
    /// Inputs are the three planes ::GpuPass::ResizeAcross wrote, sums first,
    /// with the weights of the vertical axis between the first two (so at bindings
    /// 0, 2, 3 and 4: sums, weights, low, high); uniforms are a ::arraw::GpuResizeBlock.
    ResizeDown,

    /// @brief The horizontal half of a resize of an opaque image: one render, no planes.
    ///
    /// Inputs and uniforms as ::GpuPass::ResizeAcross, whose `plane` it ignores.
    /// Writes only the premultiplied sums (which for alpha one are the filtered
    /// colour) with alpha one.
    ResizeAcrossOpaque,

    /// @brief The vertical half of a resize of an opaque image, which also produces the result.
    ///
    /// Inputs are the one image ::GpuPass::ResizeAcrossOpaque wrote and the weights
    /// of the vertical axis (bindings 0 and 2); uniforms are a ::arraw::GpuResizeBlock.
    /// Writes the filtered colour with alpha exactly one.
    ResizeDownOpaque,

    /// @brief The effects on the crop frame, after the resize; uniforms are a
    /// ::arraw::GpuEffectsBlock, the one input the resized image.
    Effects,

    /// @brief One filtering step of noise reduction, the ::arraw::DenoiseStep its
    /// ::arraw::GpuDenoiseBlock names; the one input the source or the step before's result.
    DenoiseFilter,

    /// @brief The recombination that ends noise reduction; uniforms are a
    /// ::arraw::GpuDenoiseBlock, inputs the source, the blurred ratio grid and the
    /// filtered luminance (bindings 0, 2 and 3), the source standing in for a half that is off.
    DenoiseCombine,

    /// @brief One step of one base of the Presence context, the ::arraw::PresenceStep its
    /// ::arraw::GpuPresenceBlock names; inputs the source or the step before's result, and the
    /// opened grid a floor's last blur keeps above (bindings 0 and 2), the first input standing
    /// in for the second in every other step.
    PresenceFilter,
};

/// @brief Number of ::arraw::GpuPass values, for tables indexed by one.
inline constexpr std::size_t gpuPassCount = 11;

/// @brief Channels a pass's render target stores, each a 32-bit float.
///
/// A shader writes a `vec4` either way; a target with fewer channels keeps the
/// first and drops the rest, so the same shader serves both.
enum class GpuTargetFormat {
    Rgba32F, ///< A colour and its alpha: what every image a caller sees is.
    /// @brief One channel, for a scalar intermediate such as a filtered luminance.
    ///
    /// A pass reads it as `(r, 0, 0, 1)`, and so does ::arraw::DeviceImage::readBack.
    /// On a device without R32F textures (GpuDeviceInfo::scalarFloatTextures)
    /// the target is RGBA32F instead, which reads the same in its first channel.
    R32F,
};

/// @brief Number of ::arraw::GpuTargetFormat values, for tables indexed by one.
inline constexpr std::size_t gpuTargetFormatCount = 2;

/// @brief What a pass renders into, besides its size and encoding.
struct GpuTarget {
    /// @brief Channels the target stores.
    GpuTargetFormat format = GpuTargetFormat::Rgba32F;

    /// @brief Sensor pixels per pixel of the result (::arraw::ImageBuffer::pixelScale).
    ///
    /// Empty keeps the first input's, which is right for every pass that keeps
    /// the density of the pixels; a resize gives its own.
    std::optional<double> pixelScale = std::nullopt;
};

/// @brief One graphics device, owned, offscreen, on the thread that made it.
///
/// The adapter ADR 015 describes: pass-recording code will take a device it
/// does not own, and this is the thing that owns one for callers with no
/// viewport — the command line, export, and the CPU/GPU comparison tests.
///
/// Never falls back: not to another backend, and, when an adapter was asked
/// for, not to another adapter. A backend that is not in this Qt, not on this
/// platform or will not start is an error, and so the caller learns that it has
/// no GPU rather than comparing the CPU with itself.
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
    /// @param adapter Index into ::arraw::listGpuAdapters of the adapter to use,
    /// or `std::nullopt` for the default device QRhi picks. Index 0 is also
    /// valid, and means the default device, on a backend that lists no adapters.
    /// @throws std::runtime_error if no `QGuiApplication` exists, the backend is
    /// not available in this build or on this platform, or the device cannot be
    /// created.
    /// @throws std::out_of_range if @p adapter is not below the number of
    /// adapters the backend lists; the message gives that number.
    explicit GpuContext(GpuBackend backend, std::optional<std::size_t> adapter = std::nullopt);

    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;
    GpuContext(GpuContext&&) = delete;
    GpuContext& operator=(GpuContext&&) = delete;
    ~GpuContext();

    /// @brief Identity the images this context mints carry.
    [[nodiscard]] DeviceId id() const noexcept;

    /// @brief Description of the device and what it supports.
    [[nodiscard]] const GpuDeviceInfo& info() const noexcept;

    /// @brief Whether an earlier frame failed, after which every use throws.
    [[nodiscard]] bool lost() const noexcept;

    /// @brief Counts the passes rendered so far, each of which made one texture.
    ///
    /// Uploads are not counted. A diagnostic, there for tests to say how many
    /// passes a render took (an opaque resize is two, not four) without a hook
    /// inside the pipeline. Unlike the other members it is not checked against
    /// the owner thread: a diagnostic read, only meaningful when nothing else
    /// is rendering.
    [[nodiscard]] std::size_t renderCount() const noexcept;

    /// @brief Copies a host buffer into a new RGBA32F texture.
    /// @param image Buffer to upload; must be ::arraw::PixelFormat::RgbaF32.
    /// @return A device image with the buffer's size, encoding, orientation and pixel scale.
    /// @throws std::invalid_argument if @p image is not RGBA float, or is larger
    /// than the device or a single QRhi transfer accepts.
    /// @throws std::logic_error if called from a thread other than the owner.
    /// @throws std::runtime_error if the device has no RGBA32F textures (see
    /// GpuDeviceInfo::floatTextures), cannot create or fill the texture, or an
    /// earlier transfer failed.
    [[nodiscard]] DeviceImage upload(const ImageBuffer& image);

    /// @brief Copies one plane of packed brush coverage into a new RGBA8 texture.
    ///
    /// The texture is `RGBA8` without the sRGB flag, which a driver would otherwise linearise:
    /// the codes are weights, not colours. The result reports ::arraw::PixelFormat::RgbaU8, a
    /// placeholder encoding and a pixel scale of 1; its read back is the plane. No format check:
    /// every backend has RGBA8.
    /// @param size Dimensions of the plane.
    /// @param plane `size.width * size.height * 4` bytes, RGBA, row-major.
    /// @throws std::invalid_argument if @p plane is not that long, or @p size is empty or
    /// larger than the device or a single QRhi transfer accepts.
    /// @throws std::logic_error if called from a thread other than the owner.
    /// @throws std::runtime_error if the device cannot create or fill the texture, or an earlier
    /// transfer failed.
    [[nodiscard]] DeviceImage uploadCoverage(ImageSize size, std::span<const std::uint8_t> plane);

    /// @brief Uploads rectangles of a plane into an existing coverage texture.
    ///
    /// Each rectangle is copied to a contiguous buffer; all go in one resource update batch.
    /// This is the one exception to ::arraw::DeviceImage's immutability: a coverage texture is
    /// never a checkpoint's pixels, and is updated only by the residency that made it, on the
    /// owner thread, between renders (which wait for completion).
    /// @param image A coverage texture of this device, made by ::arraw::GpuContext::uploadCoverage.
    /// @param rectangles Rectangles to copy, inside the texture; none is a no-op.
    /// @param plane The whole plane the rectangles are cut from, of the texture's size.
    /// @throws std::invalid_argument if @p image is not a coverage texture of this device,
    /// @p plane is not of its size, or a rectangle is empty or outside it.
    /// @throws std::logic_error if called from a thread other than the owner.
    /// @throws std::runtime_error if the transfer fails.
    void updateCoverage(const DeviceImage& image, std::span<const PixelRect> rectangles,
                        std::span<const std::uint8_t> plane);

    /// @brief Renders one pass from a device image into a new one.
    ///
    /// The one way a pass's output becomes a ::arraw::DeviceImage, which is
    /// what keeps this class the only minter of them. Pipelines are built on a
    /// pass's first use and kept for the device's lifetime. Waits for the
    /// render to finish, as a transfer does.
    ///
    /// When an operation is observed on this thread (ADR 042), looks for its
    /// cancellation before rendering and counts the render as a unit of its
    /// current span once done: the GPU's progress is a render at a time.
    /// @param pass Shader to run.
    /// @param uniforms The pass's uniform block, byte for byte; empty for ::GpuPass::Copy.
    /// @param input Image the pass reads; must belong to this context's device.
    /// @param outputSize Dimensions of the result.
    /// @param encoding Meaning of the result's RGB values, which the pass decides.
    /// @param target Channels of the result, and its pixel scale.
    /// @return The result, with no pending orientation.
    /// @throws std::invalid_argument if @p input is empty or belongs to another
    /// device, @p uniforms is not the pass's block size, or @p outputSize is
    /// empty or larger than the device accepts.
    /// @throws std::logic_error if called from a thread other than the owner.
    /// @throws std::runtime_error if the device has no RGBA32F textures, cannot
    /// create the pass's resources, or the render fails.
    /// @throws ::arraw::Cancelled if the operation observed on this thread is cancelled.
    [[nodiscard]] DeviceImage render(GpuPass pass, std::span<const std::byte> uniforms,
                                     const DeviceImage& input, ImageSize outputSize,
                                     const ColorEncoding& encoding, const GpuTarget& target = {});

    /// @brief Renders one pass that reads several device images into a new one.
    ///
    /// As the single-input overload, for the passes whose inputs are more than
    /// one image. The first input is bound at 0 and the rest from binding 2 on,
    /// in order, the uniform block holding binding 1.
    /// @param inputs Images the pass reads, exactly as many as it takes.
    /// @throws std::invalid_argument as the other overload, and if @p inputs is
    /// not as long as the pass takes.
    [[nodiscard]] DeviceImage render(GpuPass pass, std::span<const std::byte> uniforms,
                                     std::span<const DeviceImage> inputs, ImageSize outputSize,
                                     const ColorEncoding& encoding, const GpuTarget& target = {});

private:
    /// @brief Device shared with every image minted from it.
    std::shared_ptr<detail::GpuDevice> device_;

    /// @brief Description gathered once, when the device was created.
    GpuDeviceInfo info_;
};

/// @brief Creates a context on the default backend, unless it would be a software rasteriser.
///
/// For callers that fall back to the CPU: a rasteriser on the CPU is no faster
/// than the CPU path, and is slower to start, so it is refused as the command
/// line's auto mode refuses it. Never throws; the reason comes back instead.
/// @param problem Receives why there is no context: the failure, or
/// "Software rasteriser refused: <device>". Left untouched on success.
/// @return The context, which belongs to the calling thread, or an empty pointer.
[[nodiscard]] std::unique_ptr<GpuContext> createHardwareContext(std::string& problem);

} // namespace arraw
