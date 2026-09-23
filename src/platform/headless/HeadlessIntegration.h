#pragma once

#include "HeadlessPlatform.h"

#include <qpa/qplatformintegration.h>

#include <memory>

// QBasicPlatformVulkanInstance, which the Vulkan instance here builds on, names
// Vulkan's own function pointer types: a Vulkan-enabled Qt is not enough, the
// headers must be visible too. The same condition QRhi and arraw-gpu use.
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define ARRAW_HEADLESS_VULKAN 1
#else
#define ARRAW_HEADLESS_VULKAN 0
#endif

namespace arraw::headless {

/// @brief A Qt platform that needs no display server, for the command line on Linux.
///
/// Qt's own offscreen platform cannot create a `QVulkanInstance`, although
/// Vulkan itself needs no display; its xcb and wayland platforms can, but only
/// with a server to connect to. This one exists for the Vulkan instance alone:
/// createPlatformVulkanInstance() is its one real job. Everything else is the
/// least a `QGuiApplication` needs to start, modelled on Qt's minimal platform —
/// one screen, windows that are never shown, backing stores that are plain
/// images, and the fonts fontconfig knows so that `QPainter` can still draw text.
///
/// There is no OpenGL here: Qt makes OpenGL contexts through the GLX and EGL
/// integrations of its xcb and wayland platforms, and this one has neither. A
/// caller wanting OpenGL sets `QT_QPA_PLATFORM` to `xcb` or `wayland`.
class HeadlessIntegration final : public QPlatformIntegration {
public:
    HeadlessIntegration();
    ~HeadlessIntegration() override;

    /// @brief Announces the one screen, once `QGuiApplication` is ready for it.
    void initialize() override;

    /// @brief Reports what the platform can do: raster windows, and no OpenGL.
    [[nodiscard]] bool hasCapability(Capability capability) const override;

    /// @brief Creates the platform side of a window that is never shown.
    [[nodiscard]] QPlatformWindow* createPlatformWindow(QWindow* window) const override;

    /// @brief Creates an image a window may be painted into, which goes nowhere.
    [[nodiscard]] QPlatformBackingStore* createPlatformBackingStore(QWindow* window) const override;

    /// @brief Creates the generic Unix event dispatcher, as Qt's own headless platforms do.
    [[nodiscard]] QAbstractEventDispatcher* createEventDispatcher() const override;

    /// @brief Returns the fontconfig database, or Qt's fallback where there is none.
    [[nodiscard]] QPlatformFontDatabase* fontDatabase() const override;

#if ARRAW_HEADLESS_VULKAN
    /// @brief Creates a Vulkan instance with no surface extensions: the reason
    /// this platform exists.
    [[nodiscard]] QPlatformVulkanInstance*
    createPlatformVulkanInstance(QVulkanInstance* instance) const override;
#endif

private:
    /// @brief Fonts, created with the platform as Qt's offscreen platform does.
    std::unique_ptr<QPlatformFontDatabase> fontDatabase_;

    /// @brief The one screen, owned by Qt from initialize() until the destructor
    /// hands it back through `QWindowSystemInterface::handleScreenRemoved`.
    QPlatformScreen* screen_ = nullptr;
};

} // namespace arraw::headless
