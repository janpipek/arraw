#pragma once
#include <memory>
#include <QString>
#include <rhi/qrhi.h>

class QOffscreenSurface;

// A windowless QRhi for the CLI, the golden tests, and any headless consumer
// (docs/adr/0049). Mirrors QRhiWidget's platform-default backend so headless
// output matches the GUI on the same machine: D3D11 on Windows (WARP retry
// when the hardware device fails), Metal on macOS, OpenGL 3.3 core over a
// QOffscreenSurface elsewhere. ARRAW_RHI_BACKEND=opengl|d3d11|metal|null
// overrides for debugging; it is an escape hatch, not a supported flag.
class HeadlessRenderContext {
public:
    static std::unique_ptr<HeadlessRenderContext> create(QString* error = nullptr);
    ~HeadlessRenderContext();

    QRhi* rhi() const { return rhi_.get(); }

private:
    HeadlessRenderContext();

    std::unique_ptr<QOffscreenSurface> fallbackSurface_; // OpenGL only
    std::unique_ptr<QRhi> rhi_;
};
