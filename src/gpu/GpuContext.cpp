#include "GpuContext.h"

#include "DeviceImageState.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QSize>
#include <QString>
#include <QThread>
#include <QVersionNumber>

#include <rhi/qrhi.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

// QRhi's own condition for declaring its Vulkan parameters: a Vulkan-enabled
// Qt still hides QVulkanInstance from a build that cannot see vulkan.h.
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define ARRAW_GPU_VULKAN 1
#else
#define ARRAW_GPU_VULKAN 0
#endif

namespace arraw::detail {

/// @brief One QRhi device and everything it depends on, shared by its images.
///
/// Images hold this rather than the context, so a device image may outlive the
/// ::arraw::GpuContext that minted it. Member order is load-bearing: members are
/// destroyed in reverse, so the QRhi goes before the Vulkan instance and the
/// fallback surface it was created from, as QRhi requires.
struct GpuDevice {
#if ARRAW_GPU_VULKAN
    /// @brief Vulkan instance the device was created from, if Vulkan.
    std::unique_ptr<QVulkanInstance> vulkan;
#endif
#if QT_CONFIG(opengl)
    /// @brief Surface an OpenGL device makes its context current against, if OpenGL.
    std::unique_ptr<QOffscreenSurface> fallbackSurface;
#endif
    /// @brief The device itself.
    std::unique_ptr<QRhi> rhi;

    /// @brief Identity carried by every image the device mints.
    DeviceId id = DeviceId::None;

    /// @brief Thread that created the device, and the only one that may use it.
    std::thread::id owner = std::this_thread::get_id();

    /// @brief Refuses use from any thread but the owner.
    ///
    /// Cheap enough to do on every transfer, and the difference between an
    /// exception and a device corrupted in a way nobody can reproduce.
    /// @param action What was attempted, completing "can only ... on".
    /// @throws std::logic_error if called from another thread.
    void requireOwnerThread(const char* action) const {
        if (std::this_thread::get_id() != owner) {
            throw std::logic_error(std::string("A GPU device can only ") + action +
                                   " on the thread that created it");
        }
    }
};

} // namespace arraw::detail

namespace arraw {
namespace {

/// @brief Source of device identities; 0 is ::arraw::DeviceId::None, so it starts at 1.
std::atomic<std::uint64_t> nextDeviceId{1};

/// @brief Largest single transfer, in bytes.
///
/// QRhi's Direct3D backends size readback storage through `int`, so anything
/// larger would be truncated there rather than refused.
constexpr std::size_t maxTransferBytes = static_cast<std::size_t>(std::numeric_limits<int>::max());

/// @brief Builds the error for a backend that cannot be had.
std::runtime_error unavailable(GpuBackend backend, const std::string& reason) {
    return std::runtime_error("The " + std::string(gpuBackendName(backend)) +
                              " backend is unavailable: " + reason);
}

/// @brief Names the Qt platform plugin, which decides what a device can be made from.
std::string platformName() {
    return QGuiApplication::platformName().toStdString();
}

/// @brief Checks whether the application runs on arraw-cli's headless platform.
///
/// Named here rather than included: the key is defined beside the plugin, in
/// src/platform/headless/HeadlessPlatform.h, which exists on Linux only.
bool onHeadlessPlatform() {
    return QGuiApplication::platformName() == QLatin1String("arraw-headless");
}

/// @brief Explains a failure the platform plugin accounts for, or says nothing.
///
/// arraw-cli runs on its own headless platform on Linux, which reaches Vulkan
/// through the loader and driver alone, so a Vulkan failure there is about them.
/// Qt's offscreen platform, which someone may still choose explicitly, has no
/// Vulkan at all and OpenGL only through an X server, so a failure there is
/// about the platform rather than the GPU.
std::string platformAdvice(GpuBackend backend) {
    if (onHeadlessPlatform() && backend == GpuBackend::Vulkan) {
        return ". arraw's headless platform needs no display, only the Vulkan loader "
               "(libvulkan.so.1) and a driver: check that vulkaninfo lists a device";
    }
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        return ". Qt's offscreen platform cannot create Vulkan devices and OpenGL ones only with "
               "an X server; leave QT_QPA_PLATFORM unset for arraw-cli to use its own headless "
               "platform, which reaches Vulkan without a display, or set it to xcb or wayland "
               "inside a desktop session";
    }
    return {};
}

/// @brief Creates the QRhi for one backend, storing what it depends on in @p device.
///
/// Never passes QRhi::PreferSoftwareRenderer, and never tries a second backend
/// when the first fails: either would let a missing GPU pass for a present one
/// (ADR 015).
/// @return The device, or null if QRhi could not create one.
/// @throws std::runtime_error if the backend is not in this build or on this
/// platform, or what it depends on cannot be created.
std::unique_ptr<QRhi> createRhi(GpuBackend backend, [[maybe_unused]] detail::GpuDevice& device) {
    [[maybe_unused]] const QRhi::Flags flags{};
    switch (backend) {
    case GpuBackend::Vulkan: {
#if ARRAW_GPU_VULKAN
        auto instance = std::make_unique<QVulkanInstance>();
        // Unsupported extensions are filtered out by Qt, so the preferred list
        // is passed as is; API 1.1 unlocks features QRhi otherwise reports absent.
        instance->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (instance->supportedApiVersion() >= QVersionNumber(1, 1)) {
            instance->setApiVersion(QVersionNumber(1, 1));
        }
        if (!instance->create()) {
            throw unavailable(backend, "no Vulkan instance could be created on the '" +
                                           platformName() + "' platform (VkResult " +
                                           std::to_string(static_cast<int>(instance->errorCode())) +
                                           ")" + platformAdvice(backend));
        }
        QRhiVulkanInitParams params;
        params.inst = instance.get();
        device.vulkan = std::move(instance);
        return std::unique_ptr<QRhi>(QRhi::create(QRhi::Vulkan, &params, flags));
#else
        throw unavailable(backend,
                          "this Qt was built without Vulkan, or arraw without the Vulkan headers");
#endif
    }
    case GpuBackend::OpenGL: {
#if QT_CONFIG(opengl)
        // Refused before Qt tries, and warns about, a context the platform
        // cannot make.
        if (onHeadlessPlatform()) {
            throw unavailable(backend, "arraw's headless platform ('arraw-headless') has no "
                                       "OpenGL; set QT_QPA_PLATFORM to xcb or wayland to reach it "
                                       "through a display server");
        }
        // A fallback surface is a QOffscreenSurface, which Qt creates and
        // destroys on the GUI thread only.
        if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
            throw unavailable(backend, "an OpenGL device can only be created on the GUI thread");
        }
        device.fallbackSurface.reset(QRhiGles2InitParams::newFallbackSurface());
        QRhiGles2InitParams params;
        params.fallbackSurface = device.fallbackSurface.get();
        return std::unique_ptr<QRhi>(QRhi::create(QRhi::OpenGLES2, &params, flags));
#else
        throw unavailable(backend, "this Qt was built without OpenGL");
#endif
    }
    case GpuBackend::D3D11: {
#if defined(Q_OS_WIN)
        QRhiD3D11InitParams params;
        return std::unique_ptr<QRhi>(QRhi::create(QRhi::D3D11, &params, flags));
#else
        throw unavailable(backend, "Direct3D exists only on Windows");
#endif
    }
    case GpuBackend::D3D12: {
#if defined(Q_OS_WIN)
        QRhiD3D12InitParams params;
        return std::unique_ptr<QRhi>(QRhi::create(QRhi::D3D12, &params, flags));
#else
        throw unavailable(backend, "Direct3D exists only on Windows");
#endif
    }
    case GpuBackend::Metal: {
#if QT_CONFIG(metal)
        QRhiMetalInitParams params;
        return std::unique_ptr<QRhi>(QRhi::create(QRhi::Metal, &params, flags));
#else
        throw unavailable(backend, "Metal exists only on Apple platforms, and needs a Qt built "
                                   "with it");
#endif
    }
    }
    throw unavailable(backend, "it is not a backend arraw knows");
}

/// @brief Classifies a device, by name where the backend does not say.
///
/// QRhi's OpenGL backend reports every device as unknown, and llvmpipe is the
/// device a Linux machine without a working GPU driver quietly hands out, so
/// the known software rasterisers are recognised by the name they give.
GpuDeviceKind kindOf(const QRhiDriverInfo& driver) {
    switch (driver.deviceType) {
    case QRhiDriverInfo::IntegratedDevice:
        return GpuDeviceKind::Integrated;
    case QRhiDriverInfo::DiscreteDevice:
        return GpuDeviceKind::Discrete;
    case QRhiDriverInfo::ExternalDevice:
        return GpuDeviceKind::External;
    case QRhiDriverInfo::VirtualDevice:
        return GpuDeviceKind::Virtual;
    case QRhiDriverInfo::CpuDevice:
        return GpuDeviceKind::Software;
    case QRhiDriverInfo::UnknownDevice:
        break;
    }
    const QByteArray name = driver.deviceName.toLower();
    for (const char* rasteriser :
         {"llvmpipe", "softpipe", "lavapipe", "swiftshader", "microsoft basic render driver"}) {
        if (name.contains(rasteriser)) {
            return GpuDeviceKind::Software;
        }
    }
    return GpuDeviceKind::Unknown;
}

/// @brief Submits one batch of resource updates and waits for it to finish.
///
/// An offscreen frame, because only that promises a readback is complete when
/// it ends; the frame is always ended once begun, so a failure leaves the
/// device usable.
/// @param rhi Device to submit to.
/// @param purpose What the batch is for, as an infinitive without "to".
/// @param record Records the updates into the batch.
/// @throws std::runtime_error if the frame cannot be begun or fails.
void submitUpdates(QRhi& rhi, const std::string& purpose,
                   const std::function<void(QRhiResourceUpdateBatch&)>& record) {
    QRhiCommandBuffer* commands = nullptr;
    if (rhi.beginOffscreenFrame(&commands) != QRhi::FrameOpSuccess || commands == nullptr) {
        throw std::runtime_error("The GPU device could not begin a frame to " + purpose);
    }
    QRhiResourceUpdateBatch* batch = rhi.nextResourceUpdateBatch();
    if (batch == nullptr) {
        rhi.endOffscreenFrame();
        throw std::runtime_error("The GPU device had no resource update batch free to " + purpose);
    }
    record(*batch);
    // Commits and releases the batch; no pass is needed for transfers alone.
    commands->resourceUpdate(batch);
    if (rhi.endOffscreenFrame() != QRhi::FrameOpSuccess) {
        throw std::runtime_error("The GPU device failed to " + purpose);
    }
}

/// @brief Describes a size as `WxH`, for messages.
std::string describe(ImageSize size) {
    return std::to_string(size.width) + "x" + std::to_string(size.height);
}

/// @brief A device image whose pixels are one QRhi texture.
class RhiDeviceImage final : public detail::DeviceImageState {
public:
    RhiDeviceImage(std::shared_ptr<detail::GpuDevice> owner, std::unique_ptr<QRhiTexture> pixels,
                   ImageSize dimensions, ColorEncoding meaning, ImageOrientation source)
        : DeviceImageState(owner->id, dimensions, PixelFormat::RgbaF32, std::move(meaning), source),
          device_(std::move(owner)), texture_(std::move(pixels)) {}

    [[nodiscard]] ImageBuffer readBack() const override {
        device_->requireOwnerThread("read back");

        QRhiReadbackResult result{};
        submitUpdates(*device_->rhi, "read a texture back", [&](QRhiResourceUpdateBatch& batch) {
            batch.readBackTexture(QRhiReadbackDescription(texture_.get()), &result);
        });

        ImageBuffer buffer(size, format, encoding, orientation);
        const std::span<std::byte> destination = buffer.bytes();
        // Every backend returns rows tightly packed, which is also the
        // buffer's own layout, so one exact size check covers the stride too.
        const QSize expected(static_cast<int>(size.width), static_cast<int>(size.height));
        if (result.pixelSize != expected ||
            static_cast<std::size_t>(result.data.size()) != destination.size()) {
            throw std::runtime_error(
                "The GPU device read back " + std::to_string(result.data.size()) + " bytes of a " +
                describe(size) + " texture; expected " + std::to_string(destination.size()));
        }
        std::memcpy(destination.data(), result.data.constData(), destination.size());
        return buffer;
    }

private:
    /// @brief Device the texture belongs to; declared first so it is destroyed last.
    std::shared_ptr<detail::GpuDevice> device_;

    /// @brief The pixels.
    std::unique_ptr<QRhiTexture> texture_;
};

} // namespace

GpuBackend defaultGpuBackend() noexcept {
#if defined(_WIN32)
    return GpuBackend::D3D11;
#elif defined(__APPLE__)
    return GpuBackend::Metal;
#else
    return GpuBackend::Vulkan;
#endif
}

std::string_view gpuBackendName(GpuBackend backend) noexcept {
    switch (backend) {
    case GpuBackend::Vulkan:
        return "vulkan";
    case GpuBackend::OpenGL:
        return "opengl";
    case GpuBackend::D3D11:
        return "d3d11";
    case GpuBackend::D3D12:
        return "d3d12";
    case GpuBackend::Metal:
        return "metal";
    }
    return "unknown";
}

std::optional<GpuBackend> parseGpuBackend(std::string_view name) noexcept {
    for (const GpuBackend backend : {GpuBackend::Vulkan, GpuBackend::OpenGL, GpuBackend::D3D11,
                                     GpuBackend::D3D12, GpuBackend::Metal}) {
        if (gpuBackendName(backend) == name) {
            return backend;
        }
    }
    return std::nullopt;
}

GpuContext::GpuContext(GpuBackend backend) : device_(std::make_shared<detail::GpuDevice>()) {
    // Checked first, and for every backend alike: without it a Vulkan instance
    // or an OpenGL surface fails deep inside Qt, and a QCoreApplication is what
    // the main test suite runs under, and what arraw-cli runs under when
    // ARRAW_DISABLE_GPU turns the GPU off.
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        throw std::runtime_error("A GPU device needs a QGuiApplication, which provides the "
                                 "platform plugin devices are created through");
    }

    device_->rhi = createRhi(backend, *device_);
    if (!device_->rhi) {
        throw unavailable(backend, "QRhi could not create a device on the '" + platformName() +
                                       "' platform; Qt's warnings above say why" +
                                       platformAdvice(backend));
    }

    const QRhi& rhi = *device_->rhi;
    const QRhiDriverInfo driver = rhi.driverInfo();
    info_.backend = backend;
    info_.deviceName = driver.deviceName.toStdString();
    info_.kind = kindOf(driver);
    info_.vendorId = driver.vendorId;
    info_.deviceId = driver.deviceId;
    info_.floatTextures = rhi.isTextureFormatSupported(QRhiTexture::RGBA32F);
    info_.halfFloatTextures = rhi.isTextureFormatSupported(QRhiTexture::RGBA16F);
    info_.compute = rhi.isFeatureSupported(QRhi::Compute);
    info_.anyFormatReadBack = rhi.isFeatureSupported(QRhi::ReadBackAnyTextureFormat);
    info_.maxTextureSize = rhi.resourceLimit(QRhi::TextureSizeMax);

    // Minted last, so a device that failed to start never took an identity.
    device_->id = static_cast<DeviceId>(nextDeviceId.fetch_add(1, std::memory_order_relaxed));
}

GpuContext::~GpuContext() = default;

DeviceId GpuContext::id() const noexcept {
    return device_->id;
}

const GpuDeviceInfo& GpuContext::info() const noexcept {
    return info_;
}

DeviceImage GpuContext::upload(const ImageBuffer& image) {
    device_->requireOwnerThread("upload");

    if (image.format() != PixelFormat::RgbaF32) {
        // RGBA32F is the only texture format the device side stores, and QRhi
        // copies bytes rather than converting them, so anything else would
        // arrive as a different image.
        throw std::invalid_argument("A GPU upload takes RGBA float pixels");
    }
    if (!info_.floatTextures) {
        // Said here rather than left to texture creation, whose failure would
        // not say which of format, size or memory was the problem.
        throw std::runtime_error("This GPU device has no RGBA32F textures, which an upload needs");
    }
    const ImageSize size = image.size();
    const std::uint32_t edge =
        info_.maxTextureSize > 0 ? static_cast<std::uint32_t>(info_.maxTextureSize) : 0U;
    if (size.width > edge || size.height > edge) {
        throw std::invalid_argument("A " + describe(size) +
                                    " image exceeds this device's largest texture edge, " +
                                    std::to_string(edge) + " pixels");
    }
    const std::span<const std::byte> bytes = image.bytes();
    if (bytes.size() > maxTransferBytes) {
        throw std::invalid_argument("A " + describe(size) +
                                    " image is larger than one GPU transfer can carry");
    }

    QRhi& rhi = *device_->rhi;
    // A transfer source so that it can be read back; load/store where compute
    // exists, for the compute passes that will write into textures like it.
    QRhiTexture::Flags flags = QRhiTexture::UsedAsTransferSource;
    if (info_.compute) {
        flags |= QRhiTexture::UsedWithLoadStore;
    }
    std::unique_ptr<QRhiTexture> texture(rhi.newTexture(
        QRhiTexture::RGBA32F, QSize(static_cast<int>(size.width), static_cast<int>(size.height)), 1,
        flags));
    if (!texture->create()) {
        throw std::runtime_error("The GPU device could not create a " + describe(size) +
                                 " RGBA32F texture");
    }

    // Borrowed rather than copied: the frame below has completed, and the
    // bytes been consumed, before this function returns.
    QRhiTextureSubresourceUploadDescription subresource;
    subresource.setData(QByteArray::fromRawData(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<qsizetype>(bytes.size())));
    const QRhiTextureUploadDescription description(QRhiTextureUploadEntry(0, 0, subresource));
    submitUpdates(rhi, "upload a texture", [&](QRhiResourceUpdateBatch& batch) {
        batch.uploadTexture(texture.get(), description);
    });

    return DeviceImage(std::make_shared<const RhiDeviceImage>(
        device_, std::move(texture), size, image.encoding(), image.orientation()));
}

} // namespace arraw
