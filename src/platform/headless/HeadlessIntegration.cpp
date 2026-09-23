#include "HeadlessIntegration.h"

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QString>
#include <QtGui/private/qgenericunixeventdispatcher_p.h>
#include <QtGui/private/qgenericunixfontdatabase_p.h>
#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformfontdatabase.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>

#if ARRAW_HEADLESS_VULKAN
#include <QByteArrayList>
#include <QtGui/private/qbasicvulkanplatforminstance_p.h>
#endif

#include <cstdint>

namespace arraw::headless {
namespace {

/// @brief Pixel layout of the screen and every backing store, as Qt's minimal platform has.
constexpr QImage::Format screenFormat = QImage::Format_ARGB32_Premultiplied;

/// @brief The one screen: a fixed size, for code that asks what a screen is like.
///
/// Nothing is ever shown on it. 800x600 rather than the minimal platform's
/// 240x320 only so that a window of ordinary size fits.
class HeadlessScreen final : public QPlatformScreen {
public:
    [[nodiscard]] QRect geometry() const override {
        return {0, 0, 800, 600};
    }

    [[nodiscard]] int depth() const override {
        return 32;
    }

    [[nodiscard]] QImage::Format format() const override {
        return screenFormat;
    }

    [[nodiscard]] QString name() const override {
        return QString::fromLatin1(platformKey);
    }
};

/// @brief A window's pixels, painted into an image that nobody sees.
///
/// Enough for QPainter and QBackingStore to work against a window, which is
/// all a headless process should ever need of one.
class HeadlessBackingStore final : public QPlatformBackingStore {
public:
    explicit HeadlessBackingStore(QWindow* window) : QPlatformBackingStore(window) {}

    [[nodiscard]] QPaintDevice* paintDevice() override {
        return &image_;
    }

    void flush(QWindow* /*window*/, const QRegion& /*region*/, const QPoint& /*offset*/) override {
        // There is no display to put the pixels on.
    }

    void resize(const QSize& size, const QRegion& /*staticContents*/) override {
        if (image_.size() != size) {
            image_ = QImage(size, screenFormat);
        }
    }

private:
    /// @brief The window's pixels.
    QImage image_;
};

#if ARRAW_HEADLESS_VULKAN
/// @brief A Vulkan instance with no window-system surface extension.
///
/// QXcbVulkanInstance minus the X server: Qt's basic implementation does the
/// work — it loads the loader, filters the requested layers and extensions to
/// the supported ones (so QRhi's preferred list may be asked for as is),
/// creates the instance and destroys it — and this adds no window-system
/// surface extension. The base still enables VK_KHR_surface itself when the
/// loader lists it, and warns if its entry points are missing; a driver without
/// WSI cannot back a QRhi device on any platform, since QRhi always asks for
/// VK_KHR_swapchain. Nothing can be presented, which suits a device that
/// renders offscreen only.
class HeadlessVulkanInstance final : public QBasicPlatformVulkanInstance {
public:
    explicit HeadlessVulkanInstance(QVulkanInstance* instance) : instance_(instance) {
        // libvulkan.so.1, the loader's ABI name, as the xcb platform loads it;
        // QT_VULKAN_LIB still overrides it.
        loadVulkanLibrary(QStringLiteral("vulkan"), 1);
    }

    void createOrAdoptInstance() override {
        initInstance(instance_, QByteArrayList{});
    }

    [[nodiscard]] bool supportsPresent(VkPhysicalDevice /*physicalDevice*/,
                                       std::uint32_t /*queueFamilyIndex*/,
                                       QWindow* /*window*/) override {
        // No window here has a surface to present to.
        return false;
    }

private:
    /// @brief Instance this backs, whose layers, extensions and version are used.
    QVulkanInstance* instance_;
};
#endif

} // namespace

HeadlessIntegration::HeadlessIntegration() : fontDatabase_(new QGenericUnixFontDatabase) {}

HeadlessIntegration::~HeadlessIntegration() {
    if (screen_ != nullptr) {
        // Deletes the screen, which Qt has owned since it was added.
        QWindowSystemInterface::handleScreenRemoved(screen_);
    }
}

void HeadlessIntegration::initialize() {
    screen_ = new HeadlessScreen;
    QWindowSystemInterface::handleScreenAdded(screen_);
}

bool HeadlessIntegration::hasCapability(Capability capability) const {
    // As Qt's minimal and offscreen platforms answer, plus an explicit no to
    // every form of OpenGL: this platform can create no GL context.
    switch (capability) {
    case ThreadedPixmaps:
    case MultipleWindows:
        return true;
    case OpenGL:
    case ThreadedOpenGL:
    case RasterGLSurface:
    case OpenGLOnRasterSurface:
    case RhiBasedRendering:
        return false;
    default:
        return QPlatformIntegration::hasCapability(capability);
    }
}

QPlatformWindow* HeadlessIntegration::createPlatformWindow(QWindow* window) const {
    return new QPlatformWindow(window);
}

QPlatformBackingStore* HeadlessIntegration::createPlatformBackingStore(QWindow* window) const {
    return new HeadlessBackingStore(window);
}

QAbstractEventDispatcher* HeadlessIntegration::createEventDispatcher() const {
    return QtGenericUnixDispatcher::createUnixEventDispatcher();
}

QPlatformFontDatabase* HeadlessIntegration::fontDatabase() const {
    return fontDatabase_.get();
}

#if ARRAW_HEADLESS_VULKAN
QPlatformVulkanInstance*
HeadlessIntegration::createPlatformVulkanInstance(QVulkanInstance* instance) const {
    return new HeadlessVulkanInstance(instance);
}
#endif

} // namespace arraw::headless
