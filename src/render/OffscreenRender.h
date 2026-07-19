#pragma once
#include <QImage>

struct GlobalAdjustment;
struct ImageBuffer;
class RendererCore;

namespace offscreen {

// The export render (docs/adr/0022, 0049): one shader pass over `buf` with
// overlays off and a linear working-space readback (RGBA32F), scaled to
// (outW, outH) while still linear. The single recipe shared by the GUI
// export, the CLI export command, and the golden tests.
QImage renderToImage(
    RendererCore& core, const ImageBuffer& buf, const GlobalAdjustment& p, int outW, int outH);

// The on-screen display path (sRGB encode) with the clipping overlay on,
// for the clipping golden (docs/adr/0009).
QImage renderClipSample(
    RendererCore& core,
    const ImageBuffer& buf,
    const GlobalAdjustment& p,
    bool clipHighlights,
    bool clipShadows);

} // namespace offscreen
