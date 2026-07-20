#include "render/HeadlessRenderContext.h"
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <rhi/qrhi_platform.h>

namespace {

QRhi::Implementation defaultBackend() {
#if defined(Q_OS_WIN)
    return QRhi::D3D11;
#elif defined(Q_OS_MACOS)
    return QRhi::Metal;
#else
    return QRhi::OpenGLES2;
#endif
}

bool backendFromEnv(QRhi::Implementation* backend, QString* error) {
    const QString name = qEnvironmentVariable("ARRAW_RHI_BACKEND").toLower();
    if (name.isEmpty()) {
        *backend = defaultBackend();
        return true;
    }
    if (name == "opengl") *backend = QRhi::OpenGLES2;
    else if (name == "null") *backend = QRhi::Null;
#if defined(Q_OS_WIN)
    else if (name == "d3d11") *backend = QRhi::D3D11;
#endif
#if defined(Q_OS_MACOS)
    else if (name == "metal") *backend = QRhi::Metal;
#endif
    else {
        if (error)
            *error = QStringLiteral("unsupported ARRAW_RHI_BACKEND '%1' on this platform").arg(name);
        return false;
    }
    return true;
}

} // namespace

HeadlessRenderContext::HeadlessRenderContext() = default;
HeadlessRenderContext::~HeadlessRenderContext() = default;

std::unique_ptr<HeadlessRenderContext> HeadlessRenderContext::create(QString* error) {
    QRhi::Implementation backend;
    if (!backendFromEnv(&backend, error))
        return nullptr;

    auto ctx = std::unique_ptr<HeadlessRenderContext>(new HeadlessRenderContext);
    switch (backend) {
    case QRhi::OpenGLES2: {
        // Match the GUI's 3.3 core request (src/GuiMain.cpp) so the shader
        // translation path is identical. Qt 6.11 dropped
        // QRhiGles2InitParams::adjustedFormat() (present in some earlier 6.x
        // releases); building the format explicitly is equivalent here since
        // version/profile are overwritten immediately after anyway.
        QSurfaceFormat fmt;
        fmt.setVersion(3, 3);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
        QRhiGles2InitParams params;
        params.format = fmt;
        ctx->fallbackSurface_.reset(QRhiGles2InitParams::newFallbackSurface(fmt));
        params.fallbackSurface = ctx->fallbackSurface_.get();
        ctx->rhi_.reset(QRhi::create(QRhi::OpenGLES2, &params));
        break;
    }
#if defined(Q_OS_WIN)
    case QRhi::D3D11: {
        QRhiD3D11InitParams params;
        ctx->rhi_.reset(QRhi::create(QRhi::D3D11, &params));
        if (!ctx->rhi_) // no hardware device (Server Core, CI): WARP
            ctx->rhi_.reset(QRhi::create(QRhi::D3D11, &params, QRhi::PreferSoftwareRenderer));
        break;
    }
#endif
#if defined(Q_OS_MACOS)
    case QRhi::Metal: {
        QRhiMetalInitParams params;
        ctx->rhi_.reset(QRhi::create(QRhi::Metal, &params));
        break;
    }
#endif
    case QRhi::Null: {
        QRhiNullInitParams params;
        ctx->rhi_.reset(QRhi::create(QRhi::Null, &params));
        break;
    }
    default:
        break;
    }

    if (!ctx->rhi_) {
        if (error && error->isEmpty())
            *error = QStringLiteral("could not create a GPU device (try QT_QPA_PLATFORM=offscreen "
                                    "on a display-less machine, or ARRAW_RHI_BACKEND=opengl)");
        return nullptr;
    }
    return ctx;
}
