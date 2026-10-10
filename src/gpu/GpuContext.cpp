#include "GpuContext.h"

#include "DeviceImageState.h"
#include "GpuDevice.h"
#include "GpuPlan.h"
#include "ProgressScope.h"
#include "TimingTrace.h"

#include <ColorEncoding.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QThread>
#include <QVersionNumber>
#include <QtAlgorithms>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

/// @brief Prepares what one backend's QRhi is created from and calls @p use with it.
///
/// The one place a backend's init parameters are made, so that enumerating its
/// adapters and creating a device from one see the same instance or surface.
/// What the parameters depend on is stored in @p device, which must outlive
/// anything @p use keeps.
/// @param use Receives the QRhi implementation and its parameters.
/// @throws std::runtime_error if the backend is not in this build or on this
/// platform, or what it depends on cannot be created.
void withInitParams(GpuBackend backend, [[maybe_unused]] detail::GpuDevice& device,
                    const std::function<void(QRhi::Implementation, QRhiInitParams*)>& use) {
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
        use(QRhi::Vulkan, &params);
        return;
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
        use(QRhi::OpenGLES2, &params);
        return;
#else
        throw unavailable(backend, "this Qt was built without OpenGL");
#endif
    }
    case GpuBackend::D3D11: {
#if defined(Q_OS_WIN)
        QRhiD3D11InitParams params;
        use(QRhi::D3D11, &params);
        return;
#else
        throw unavailable(backend, "Direct3D exists only on Windows");
#endif
    }
    case GpuBackend::D3D12: {
#if defined(Q_OS_WIN)
        QRhiD3D12InitParams params;
        use(QRhi::D3D12, &params);
        return;
#else
        throw unavailable(backend, "Direct3D exists only on Windows");
#endif
    }
    case GpuBackend::Metal: {
#if QT_CONFIG(metal)
        QRhiMetalInitParams params;
        use(QRhi::Metal, &params);
        return;
#else
        throw unavailable(backend, "Metal exists only on Apple platforms, and needs a Qt built "
                                   "with it");
#endif
    }
    }
    throw unavailable(backend, "it is not a backend arraw knows");
}

/// @brief Owns the adapters QRhi enumerated, and frees them.
struct AdapterList {
    QRhi::AdapterList adapters;

    explicit AdapterList(QRhi::AdapterList enumerated) : adapters(std::move(enumerated)) {}
    AdapterList(const AdapterList&) = delete;
    AdapterList& operator=(const AdapterList&) = delete;
    ~AdapterList() {
        qDeleteAll(adapters);
    }
};

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

/// @brief Describes a size as `WxH`, for messages.
std::string describe(ImageSize size) {
    return std::to_string(size.width) + "x" + std::to_string(size.height);
}

/// @brief A device image whose pixels are one QRhi texture.
class RhiDeviceImage final : public detail::DeviceImageState {
public:
    RhiDeviceImage(std::shared_ptr<detail::GpuDevice> owner, std::unique_ptr<QRhiTexture> pixels,
                   ImageSize dimensions, PixelFormat layout, ColorEncoding meaning,
                   ImageOrientation source, double scale)
        : DeviceImageState(owner->id, dimensions, layout, std::move(meaning), source, scale,
                           pixels->format() == QRhiTexture::R32F ? 1 : 4),
          device_(std::move(owner)), texture_(std::move(pixels)) {}

    RhiDeviceImage(const RhiDeviceImage&) = delete;
    RhiDeviceImage& operator=(const RhiDeviceImage&) = delete;
    RhiDeviceImage(RhiDeviceImage&&) = delete;
    RhiDeviceImage& operator=(RhiDeviceImage&&) = delete;

    /// Releases the texture, and the device with it if this held the last
    /// share, which QRhi allows only on the owner thread. A destructor cannot
    /// refuse, so a debug build stops here rather than corrupt the device.
    ~RhiDeviceImage() override {
        assert(device_->onOwnerThread() &&
               "a device image's last copy must be released on its device's owner thread");
    }

    /// @brief The pixels, for a pass on the same device to read.
    [[nodiscard]] QRhiTexture& texture() const noexcept {
        return *texture_;
    }

    [[nodiscard]] ImageBuffer readBack() const override {
        const detail::TimingSpan timing("gpu.readback");
        device_->requireUsable("read back");

        // On the heap, because a failed frame leaves QRhi holding its address;
        // the device then keeps it for as long as QRhi might write to it.
        auto pending = std::make_unique<QRhiReadbackResult>();
        try {
            submitUpdates(*device_, "read a texture back", [&](QRhiResourceUpdateBatch& batch) {
                batch.readBackTexture(QRhiReadbackDescription(texture_.get()), pending.get());
            });
        } catch (...) {
            device_->abandonedReadbacks.push_back(std::move(pending));
            throw;
        }
        const QRhiReadbackResult& result = *pending;

        ImageBuffer buffer(size, format, encoding, orientation);
        buffer.setPixelScale(pixelScale);
        const std::span<std::byte> destination = buffer.bytes();
        // Every backend returns rows tightly packed, which is also the
        // buffer's own layout, so one exact size check covers the stride too.
        // A coverage texture is RGBA8, whose four bytes a texel are the buffer's own.
        const std::size_t expectedBytes =
            format == PixelFormat::RgbaU8 ? destination.size() : destination.size() / 4 * channels;
        const QSize expected(static_cast<int>(size.width), static_cast<int>(size.height));
        if (result.pixelSize != expected ||
            static_cast<std::size_t>(result.data.size()) != expectedBytes) {
            throw std::runtime_error(
                "The GPU device read back " + std::to_string(result.data.size()) + " bytes of a " +
                describe(size) + " texture; expected " + std::to_string(expectedBytes));
        }
        if (channels == 4) {
            std::memcpy(destination.data(), result.data.constData(), destination.size());
            return buffer;
        }
        // A scalar texture reads as a pass samples it: (r, 0, 0, 1).
        const auto samples = buffer.samples<float>();
        for (std::size_t pixel = 0; pixel < samples.size() / 4; ++pixel) {
            std::memcpy(&samples[pixel * 4], result.data.constData() + pixel * sizeof(float),
                        sizeof(float));
            samples[pixel * 4 + 3] = 1.0F;
        }
        return buffer;
    }

private:
    /// @brief Device the texture belongs to; declared first so it is destroyed last.
    std::shared_ptr<detail::GpuDevice> device_;

    /// @brief The pixels.
    std::unique_ptr<QRhiTexture> texture_;
};

/// @brief Names a pass, for messages.
std::string_view passName(GpuPass pass) {
    switch (pass) {
    case GpuPass::Copy:
        return "copy";
    case GpuPass::Pointwise:
        return "pointwise";
    case GpuPass::Geometry:
        return "geometry";
    case GpuPass::ResizeAcross:
        return "resize-across";
    case GpuPass::ResizeDown:
        return "resize-down";
    case GpuPass::ResizeAcrossOpaque:
        return "resize-across-opaque";
    case GpuPass::ResizeDownOpaque:
        return "resize-down-opaque";
    case GpuPass::Effects:
        return "effects";
    case GpuPass::DenoiseFilter:
        return "denoise-filter";
    case GpuPass::DenoiseCombine:
        return "denoise-combine";
    case GpuPass::PresenceFilter:
        return "presence-filter";
    }
    return "unknown";
}

/// @brief Resource path of a pass's compiled fragment shader.
QString fragmentShaderOf(GpuPass pass) {
    switch (pass) {
    case GpuPass::Copy:
        return QStringLiteral(":/arraw/shaders/copy.frag.qsb");
    case GpuPass::Pointwise:
        return QStringLiteral(":/arraw/shaders/develop.frag.qsb");
    case GpuPass::Geometry:
        return QStringLiteral(":/arraw/shaders/geometry.frag.qsb");
    case GpuPass::ResizeAcross:
        return QStringLiteral(":/arraw/shaders/resize_across.frag.qsb");
    case GpuPass::ResizeDown:
        return QStringLiteral(":/arraw/shaders/resize_down.frag.qsb");
    case GpuPass::ResizeAcrossOpaque:
        return QStringLiteral(":/arraw/shaders/resize_across_opaque.frag.qsb");
    case GpuPass::ResizeDownOpaque:
        return QStringLiteral(":/arraw/shaders/resize_down_opaque.frag.qsb");
    case GpuPass::Effects:
        return QStringLiteral(":/arraw/shaders/effects.frag.qsb");
    case GpuPass::DenoiseFilter:
        return QStringLiteral(":/arraw/shaders/denoise_filter.frag.qsb");
    case GpuPass::DenoiseCombine:
        return QStringLiteral(":/arraw/shaders/denoise_combine.frag.qsb");
    case GpuPass::PresenceFilter:
        return QStringLiteral(":/arraw/shaders/presence_filter.frag.qsb");
    }
    return {};
}

/// @brief Size of the uniform block a pass reads, in bytes; zero for none.
std::size_t uniformSizeOf(GpuPass pass) {
    switch (pass) {
    case GpuPass::Copy:
        return 0;
    case GpuPass::Pointwise:
        return sizeof(GpuPointwiseBlock);
    case GpuPass::Geometry:
        return sizeof(GpuGeometryBlock);
    case GpuPass::ResizeAcross:
    case GpuPass::ResizeDown:
    case GpuPass::ResizeAcrossOpaque:
    case GpuPass::ResizeDownOpaque:
        return sizeof(GpuResizeBlock);
    case GpuPass::Effects:
        return sizeof(GpuEffectsBlock);
    case GpuPass::DenoiseFilter:
    case GpuPass::DenoiseCombine:
        return sizeof(GpuDenoiseBlock);
    case GpuPass::PresenceFilter:
        return sizeof(GpuPresenceBlock);
    }
    return 0;
}

/// @brief Number of images a pass reads.
std::size_t inputCountOf(GpuPass pass) {
    switch (pass) {
    case GpuPass::Copy:
    case GpuPass::Geometry:
    case GpuPass::Effects:
    case GpuPass::DenoiseFilter:
        return 1;
    case GpuPass::DenoiseCombine:
        return 3;
    case GpuPass::Pointwise:
        return 11;
    case GpuPass::ResizeAcross:
    case GpuPass::ResizeAcrossOpaque:
    case GpuPass::ResizeDownOpaque:
    case GpuPass::PresenceFilter:
        return 2;
    case GpuPass::ResizeDown:
        return 4;
    }
    return 1;
}

/// @brief Loads a shader compiled into this build by qt_add_shaders.
/// @throws std::runtime_error if it is missing or unreadable, which is a
/// build fault rather than a device's.
QShader loadShader(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("The GPU shader " + path.toStdString() +
                                 " is missing from this build");
    }
    QShader shader = QShader::fromSerialized(file.readAll());
    if (!shader.isValid()) {
        throw std::runtime_error("The GPU shader " + path.toStdString() + " cannot be read");
    }
    return shader;
}

/// @brief Binds a pass's first input at 0, its uniform block at 1 if it has one, and the
/// other inputs from 2 on.
///
/// With null resources it describes only the layout, which is what a pipeline
/// is created against; each render binds its own resources in the same layout.
std::vector<QRhiShaderResourceBinding> bindingsFor(std::span<QRhiTexture* const> inputs,
                                                   QRhiSampler* sampler, QRhiBuffer* uniforms,
                                                   bool hasUniforms) {
    constexpr auto stage = QRhiShaderResourceBinding::FragmentStage;
    std::vector<QRhiShaderResourceBinding> bindings{
        QRhiShaderResourceBinding::sampledTexture(0, stage, inputs.front(), sampler)};
    if (hasUniforms) {
        bindings.push_back(QRhiShaderResourceBinding::uniformBuffer(1, stage, uniforms));
    }
    for (std::size_t index = 1; index < inputs.size(); ++index) {
        bindings.push_back(QRhiShaderResourceBinding::sampledTexture(
            static_cast<int>(index) + 1, stage, inputs[index], sampler));
    }
    return bindings;
}

/// @brief Builds a pass's pipeline against the render pass its targets share.
/// @throws std::runtime_error if a shader is missing or the pipeline cannot be created.
void buildPipeline(QRhi& rhi, GpuPass pass, detail::PassPipeline& cached) {
    const std::vector<QRhiShaderResourceBinding> layout =
        bindingsFor(std::vector<QRhiTexture*>(inputCountOf(pass), nullptr), nullptr, nullptr,
                    uniformSizeOf(pass) > 0);
    std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi.newShaderResourceBindings());
    bindings->setBindings(layout.begin(), layout.end());
    if (!bindings->create()) {
        throw std::runtime_error("The GPU device could not lay out the " +
                                 std::string(passName(pass)) + " pass's resources");
    }

    std::unique_ptr<QRhiGraphicsPipeline> pipeline(rhi.newGraphicsPipeline());
    pipeline->setShaderStages({{QRhiShaderStage::Vertex,
                                loadShader(QStringLiteral(":/arraw/shaders/fullscreen.vert.qsb"))},
                               {QRhiShaderStage::Fragment, loadShader(fragmentShaderOf(pass))}});
    // The fullscreen triangle is made from the vertex index: no vertex input.
    pipeline->setVertexInputLayout({});
    pipeline->setShaderResourceBindings(bindings.get());
    pipeline->setRenderPassDescriptor(cached.renderPass.get());
    if (!pipeline->create()) {
        throw std::runtime_error("The GPU device could not create the " +
                                 std::string(passName(pass)) + " pass's pipeline");
    }
    cached.layout = std::move(bindings);
    cached.pipeline = std::move(pipeline);
}

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

bool gpuUsedByDefault() noexcept {
#if defined(_WIN32)
    return false;
#else
    return true;
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

namespace {

/// @brief Refuses a caller that has no platform plugin to make devices through.
///
/// Checked first, and for every backend alike: without it a Vulkan instance
/// or an OpenGL surface fails deep inside Qt, and a QCoreApplication is what
/// the main test suite runs under, and what arraw-cli runs under when
/// ARRAW_DISABLE_GPU turns the GPU off.
void requireGuiApplication() {
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        throw std::runtime_error("A GPU device needs a QGuiApplication, which provides the "
                                 "platform plugin devices are created through");
    }
}

} // namespace

std::vector<GpuAdapterInfo> listGpuAdapters(GpuBackend backend) {
    requireGuiApplication();

    // Scratch: keeps the instance or surface the enumeration needs alive, and
    // is dropped with the list, since no device outlives this call.
    detail::GpuDevice scratch;
    std::vector<GpuAdapterInfo> result;
    withInitParams(backend, scratch, [&](QRhi::Implementation impl, QRhiInitParams* params) {
        const AdapterList list{QRhi::enumerateAdapters(impl, params)};
        for (const QRhiAdapter* adapter : list.adapters) {
            const QRhiDriverInfo driver = adapter->info();
            result.push_back({driver.deviceName.toStdString(), kindOf(driver), driver.vendorId,
                              driver.deviceId});
        }
    });
    return result;
}

GpuContext::GpuContext(GpuBackend backend, std::optional<std::size_t> adapter)
    : device_(std::make_shared<detail::GpuDevice>()) {
    const detail::TimingSpan timing("gpu.create-context", 0, gpuBackendName(backend));
    requireGuiApplication();

    // Never QRhi::PreferSoftwareRenderer, and never a second backend when the
    // first fails: either would let a missing GPU pass for a present one (ADR 015).
    withInitParams(backend, *device_, [&](QRhi::Implementation impl, QRhiInitParams* params) {
        const QRhi::Flags flags{};
        if (!adapter) {
            device_->rhi.reset(QRhi::create(impl, params, flags));
            return;
        }
        // Enumerated with the parameters the device is created with, so the
        // index means the same adapter in both.
        const AdapterList list{QRhi::enumerateAdapters(impl, params)};
        const auto count = static_cast<std::size_t>(list.adapters.size());
        if (count == 0 && *adapter == 0) {
            // A backend that does not enumerate has one device, the default.
            device_->rhi.reset(QRhi::create(impl, params, flags));
            return;
        }
        if (*adapter >= count) {
            throw std::out_of_range(
                "The " + std::string(gpuBackendName(backend)) + " backend has no adapter " +
                std::to_string(*adapter) + "; " +
                (count == 0 ? std::string("it does not list adapters, so only 0 (the default "
                                          "device) is valid")
                            : "it lists " + std::to_string(count) + ", numbered 0 to " +
                                  std::to_string(count - 1)));
        }
        device_->rhi.reset(QRhi::create(impl, params, flags, nullptr,
                                        list.adapters[static_cast<qsizetype>(*adapter)]));
    });
    if (!device_->rhi) {
        throw unavailable(backend, "QRhi could not create a device on the '" + platformName() +
                                       "' platform; Qt's warnings above say why" +
                                       platformAdvice(backend));
    }

    const QRhi& rhi = *device_->rhi;
    const QRhiDriverInfo driver = rhi.driverInfo();
    info_.backend = backend;
    info_.adapter = adapter;
    info_.deviceName = driver.deviceName.toStdString();
    info_.kind = kindOf(driver);
    info_.vendorId = driver.vendorId;
    info_.deviceId = driver.deviceId;
    info_.floatTextures = rhi.isTextureFormatSupported(QRhiTexture::RGBA32F);
    info_.halfFloatTextures = rhi.isTextureFormatSupported(QRhiTexture::RGBA16F);
    info_.scalarFloatTextures = rhi.isTextureFormatSupported(QRhiTexture::R32F);
    info_.compute = rhi.isFeatureSupported(QRhi::Compute);
    info_.anyFormatReadBack = rhi.isFeatureSupported(QRhi::ReadBackAnyTextureFormat);
    info_.maxTextureSize = rhi.resourceLimit(QRhi::TextureSizeMax);

    // Minted last, so a device that failed to start never took an identity.
    device_->id = static_cast<DeviceId>(nextDeviceId.fetch_add(1, std::memory_order_relaxed));
}

GpuContext::~GpuContext() {
    // As for an image: this may be the device's last share.
    assert(device_->onOwnerThread() &&
           "a GPU context must be destroyed on the thread that created it");
}

DeviceId GpuContext::id() const noexcept {
    return device_->id;
}

const GpuDeviceInfo& GpuContext::info() const noexcept {
    return info_;
}

bool GpuContext::lost() const noexcept {
    return device_->isLost();
}

DeviceImage GpuContext::upload(const ImageBuffer& image) {
    const detail::TimingSpan timing("gpu.upload");
    device_->requireUsable("upload");

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
    submitUpdates(*device_, "upload a texture", [&](QRhiResourceUpdateBatch& batch) {
        batch.uploadTexture(texture.get(), description);
    });

    return DeviceImage(std::make_shared<const RhiDeviceImage>(
        device_, std::move(texture), size, PixelFormat::RgbaF32, image.encoding(),
        image.orientation(), image.pixelScale()));
}

DeviceImage GpuContext::uploadCoverage(ImageSize size, std::span<const std::uint8_t> plane) {
    const detail::TimingSpan timing("gpu.upload-coverage");
    device_->requireUsable("upload");

    const std::uint32_t edge =
        info_.maxTextureSize > 0 ? static_cast<std::uint32_t>(info_.maxTextureSize) : 0U;
    if (size.empty() || size.width > edge || size.height > edge) {
        throw std::invalid_argument("A " + describe(size) +
                                    " coverage plane exceeds this device's largest texture edge, " +
                                    std::to_string(edge) + " pixels");
    }
    if (plane.size() != size.pixelCount() * 4) {
        throw std::invalid_argument("A coverage plane of " + describe(size) + " holds " +
                                    std::to_string(size.pixelCount() * 4) + " bytes, not " +
                                    std::to_string(plane.size()));
    }
    if (plane.size() > maxTransferBytes) {
        throw std::invalid_argument("A " + describe(size) +
                                    " coverage plane is larger than one GPU transfer can carry");
    }

    QRhi& rhi = *device_->rhi;
    // Not sRGB: a driver would linearise the codes, which are weights and not colours.
    std::unique_ptr<QRhiTexture> texture(rhi.newTexture(
        QRhiTexture::RGBA8, QSize(static_cast<int>(size.width), static_cast<int>(size.height)), 1,
        QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) {
        throw std::runtime_error("The GPU device could not create a " + describe(size) +
                                 " RGBA8 coverage texture");
    }
    QRhiTextureSubresourceUploadDescription subresource;
    subresource.setData(QByteArray::fromRawData(reinterpret_cast<const char*>(plane.data()),
                                                static_cast<qsizetype>(plane.size())));
    const QRhiTextureUploadDescription description(QRhiTextureUploadEntry(0, 0, subresource));
    submitUpdates(*device_, "upload a coverage texture", [&](QRhiResourceUpdateBatch& batch) {
        batch.uploadTexture(texture.get(), description);
    });
    // The encoding is a placeholder: the codes are weights, not colours.
    return DeviceImage(std::make_shared<const RhiDeviceImage>(
        device_, std::move(texture), size, PixelFormat::RgbaU8, ColorEncoding(workingEncoding),
        ImageOrientation::Normal, 1.0));
}

void GpuContext::updateCoverage(const DeviceImage& image, std::span<const PixelRect> rectangles,
                                std::span<const std::uint8_t> plane) {
    const detail::TimingSpan timing("gpu.update-coverage");
    device_->requireUsable("upload");
    if (!image.valid() || image.device() != id() || image.format() != PixelFormat::RgbaU8) {
        throw std::invalid_argument(
            "Only a coverage texture of this device can be updated with coverage");
    }
    const ImageSize size = image.size();
    if (plane.size() != size.pixelCount() * 4) {
        throw std::invalid_argument("A coverage plane of " + describe(size) + " holds " +
                                    std::to_string(size.pixelCount() * 4) + " bytes, not " +
                                    std::to_string(plane.size()));
    }
    if (rectangles.empty()) {
        return;
    }
    // Each rectangle is copied out of the plane to the contiguous rows QRhi takes, and kept
    // here until the batch has completed.
    std::vector<QByteArray> pieces;
    pieces.reserve(rectangles.size());
    std::vector<QRhiTextureUploadEntry> entries;
    entries.reserve(rectangles.size());
    std::size_t total = 0;
    for (const PixelRect& rect : rectangles) {
        if (rect.width == 0 || rect.height == 0 || rect.x > size.width ||
            rect.width > size.width - rect.x || rect.y > size.height ||
            rect.height > size.height - rect.y) {
            throw std::invalid_argument("A coverage rectangle lies outside its texture");
        }
        QByteArray piece(
            static_cast<qsizetype>(static_cast<std::size_t>(rect.width) * rect.height * 4),
            Qt::Uninitialized);
        const std::size_t rowBytes = static_cast<std::size_t>(rect.width) * 4;
        for (std::uint32_t row = 0; row < rect.height; ++row) {
            std::memcpy(piece.data() + static_cast<std::size_t>(row) * rowBytes,
                        plane.data() +
                            (static_cast<std::size_t>(rect.y + row) * size.width + rect.x) * 4,
                        rowBytes);
        }
        total += static_cast<std::size_t>(piece.size());
        QRhiTextureSubresourceUploadDescription subresource;
        subresource.setData(piece);
        subresource.setSourceSize(
            QSize(static_cast<int>(rect.width), static_cast<int>(rect.height)));
        subresource.setDestinationTopLeft(
            QPoint(static_cast<int>(rect.x), static_cast<int>(rect.y)));
        entries.emplace_back(0, 0, subresource);
        pieces.push_back(std::move(piece));
    }
    if (total > maxTransferBytes) {
        throw std::invalid_argument("The coverage rectangles are larger than one GPU transfer can "
                                    "carry");
    }
    QRhiTexture& texture = static_cast<const RhiDeviceImage&>(*image.state_).texture();
    QRhiTextureUploadDescription description;
    description.setEntries(entries.begin(), entries.end());
    submitUpdates(*device_, "update a coverage texture", [&](QRhiResourceUpdateBatch& batch) {
        batch.uploadTexture(&texture, description);
    });
}

DeviceImage GpuContext::render(GpuPass pass, std::span<const std::byte> uniforms,
                               const DeviceImage& input, ImageSize outputSize,
                               const ColorEncoding& encoding, const GpuTarget& target) {
    return render(pass, uniforms, std::span(&input, 1), outputSize, encoding, target);
}

DeviceImage GpuContext::render(GpuPass pass, std::span<const std::byte> uniforms,
                               std::span<const DeviceImage> inputs, ImageSize outputSize,
                               const ColorEncoding& encoding, const GpuTarget& target) {
    // Between renders is where a GPU development notices a cancellation (ADR 042).
    detail::throwIfCancelled();
    device_->requireUsable("render");

    const auto index = static_cast<std::size_t>(pass);
    if (index >= gpuPassCount) {
        throw std::invalid_argument("Unknown GPU pass");
    }
    const std::string name(passName(pass));
    const detail::TimingSpan timing("gpu.pass", 0, name);
    if (inputs.size() != inputCountOf(pass)) {
        throw std::invalid_argument("The " + name + " pass reads " +
                                    std::to_string(inputCountOf(pass)) + " images, not " +
                                    std::to_string(inputs.size()));
    }
    for (const DeviceImage& input : inputs) {
        if (!input.valid()) {
            throw std::invalid_argument("The " + name + " pass needs an input image");
        }
        if (input.device() != id()) {
            // Textures are meaningless to any device but the one that made them
            // (ADR 015), which is also what makes the cast below sound.
            throw std::invalid_argument("The " + name +
                                        " pass can only read an image from its own device");
        }
    }
    if (uniforms.size() != uniformSizeOf(pass)) {
        throw std::invalid_argument("The " + name + " pass takes " +
                                    std::to_string(uniformSizeOf(pass)) +
                                    " bytes of uniforms, not " + std::to_string(uniforms.size()));
    }
    const std::uint32_t edge =
        info_.maxTextureSize > 0 ? static_cast<std::uint32_t>(info_.maxTextureSize) : 0U;
    if (outputSize.empty() || outputSize.width > edge || outputSize.height > edge) {
        throw std::invalid_argument("The " + name + " pass cannot render a " +
                                    describe(outputSize) +
                                    " image on a device whose largest "
                                    "texture edge is " +
                                    std::to_string(edge) + " pixels");
    }
    const double pixelScale = target.pixelScale.value_or(inputs.front().pixelScale());
    if (!std::isfinite(pixelScale) || !(pixelScale > 0.0)) {
        throw std::invalid_argument("The " + name +
                                    " pass's result needs a finite pixel scale above zero");
    }
    // A scalar target the device cannot store falls back to RGBA32F, which a
    // pass reads the same in its first channel.
    const bool scalar = target.format == GpuTargetFormat::R32F && info_.scalarFloatTextures;
    const QRhiTexture::Format format = scalar ? QRhiTexture::R32F : QRhiTexture::RGBA32F;
    const char* formatName = scalar ? " R32F" : " RGBA32F";
    if (outputSize.pixelCount() * bytesPerPixel(PixelFormat::RgbaF32) > maxTransferBytes) {
        // Refused before rendering rather than when the result is read back.
        throw std::invalid_argument("A " + describe(outputSize) +
                                    " result is larger than one GPU transfer can carry");
    }
    if (!info_.floatTextures) {
        throw std::runtime_error("This GPU device has no RGBA32F textures, which a pass needs");
    }

    std::vector<QRhiTexture*> textures;
    for (const DeviceImage& input : inputs) {
        textures.push_back(&static_cast<const RhiDeviceImage&>(*input.state_).texture());
    }
    QRhi& rhi = *device_->rhi;

    std::unique_ptr<QRhiTexture> texture(rhi.newTexture(
        format, QSize(static_cast<int>(outputSize.width), static_cast<int>(outputSize.height)), 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) {
        throw std::runtime_error("The GPU device could not create a " + describe(outputSize) +
                                 formatName + " render target");
    }
    std::unique_ptr<QRhiTextureRenderTarget> renderTarget(
        rhi.newTextureRenderTarget(QRhiTextureRenderTargetDescription(texture.get())));
    // One pipeline per pass and attachment format: a render pass is only
    // compatible with targets of its own format.
    detail::PassPipeline& cached =
        device_->passes.at(index * gpuTargetFormatCount + (scalar ? 1 : 0));
    if (!cached.renderPass) {
        // Every target of this pass and format has the same one attachment,
        // so the first one's render pass is compatible with all that follow.
        cached.renderPass.reset(renderTarget->newCompatibleRenderPassDescriptor());
    }
    renderTarget->setRenderPassDescriptor(cached.renderPass.get());
    if (!renderTarget->create()) {
        throw std::runtime_error("The GPU device could not create a " + describe(outputSize) +
                                 " render target");
    }
    if (!cached.pipeline) {
        buildPipeline(rhi, pass, cached);
    }

    if (!device_->sampler) {
        device_->sampler.reset(rhi.newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                              QRhiSampler::None, QRhiSampler::ClampToEdge,
                                              QRhiSampler::ClampToEdge));
        if (!device_->sampler->create()) {
            device_->sampler.reset();
            throw std::runtime_error("The GPU device could not create a sampler");
        }
    }
    std::unique_ptr<QRhiBuffer> buffer;
    if (!uniforms.empty()) {
        buffer.reset(rhi.newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                   static_cast<quint32>(uniforms.size())));
        if (!buffer->create()) {
            throw std::runtime_error("The GPU device could not create the " + name +
                                     " pass's uniform buffer");
        }
    }
    const std::vector<QRhiShaderResourceBinding> list =
        bindingsFor(textures, device_->sampler.get(), buffer.get(), buffer != nullptr);
    std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi.newShaderResourceBindings());
    bindings->setBindings(list.begin(), list.end());
    if (!bindings->create()) {
        throw std::runtime_error("The GPU device could not bind the " + name + " pass's resources");
    }

    detail::submitPass(*device_, "render the " + name + " pass", *renderTarget, *cached.pipeline,
                       *bindings, [&](QRhiResourceUpdateBatch& batch) {
                           if (buffer) {
                               batch.updateDynamicBuffer(buffer.get(), 0,
                                                         static_cast<quint32>(uniforms.size()),
                                                         uniforms.data());
                           }
                       });

    ++device_->rendersDone;
    DeviceImage result(std::make_shared<const RhiDeviceImage>(
        device_, std::move(texture), outputSize, PixelFormat::RgbaF32, encoding,
        ImageOrientation::Normal, pixelScale));
    // Each render is a unit of the observed step it is part of.
    detail::completeUnit();
    return result;
}

std::size_t GpuContext::renderCount() const noexcept {
    return device_->rendersDone;
}

std::unique_ptr<GpuContext> createHardwareContext(std::string& problem) {
    try {
        auto context = std::make_unique<GpuContext>(defaultGpuBackend());
        if (context->info().kind == GpuDeviceKind::Software) {
            problem = "Software rasteriser refused: " + context->info().deviceName;
            return nullptr;
        }
        return context;
    } catch (const std::exception& error) {
        problem = error.what();
    } catch (...) {
        problem = "Unknown error while creating the GPU device";
    }
    return nullptr;
}

} // namespace arraw
