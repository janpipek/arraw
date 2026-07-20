#include "render/OffscreenRender.h"
#include "core/ImageBuffer.h"
#include "core/Orientation.h"
#include "develop/CurveLut.h"
#include "develop/GlobalAdjustment.h"
#include "render/RendererCore.h"
#include <algorithm>

namespace offscreen {

QImage renderToImage(
    RendererCore& core, const ImageBuffer& buf, const GlobalAdjustment& p, int outW, int outH) {
    if (!core.ready() || !buf.valid())
        return {};

    // The curve LUT belongs to the params being rendered — not to whatever
    // image the viewport happens to show (fixes the stale-LUT batch export).
    core.setCurveLut(curveLutRgba(p));

    const QRectF& cr = p.cropRect;
    // The crop is normalised in the oriented display frame, so an odd quarter-turn
    // presents the buffer with width/height swapped (docs/adr/0029). Size the
    // offscreen target and the rotation aspect to the oriented frame, else the
    // oriented content is squished into a native-shaped texture.
    int orientedW = buf.width;
    int orientedH = buf.height;
    if (orient::swapsAspect(p.orientation))
        std::swap(orientedW, orientedH);
    const int cropW = (std::max) (1, int(cr.width() * orientedW + 0.5f));
    const int cropH = (std::max) (1, int(cr.height() * orientedH + 0.5f));

    // Offscreen target at cropped pixel size. Float format: the readback stays
    // in linear working space; the output transform happens on the CPU (lcms2).
    RendererCore::FrameParams fp;
    fp.transform = QVector4D(1.0f, 1.0f, 0.0f, 0.0f);
    fp.cropRect = cr;
    fp.aspect = float(orientedW) / float(orientedH);
    fp.baseLook = true;
    fp.displayEncode = false;
    fp.curveInput = false;
    fp.useLut = false;
    fp.gamutWarn = false;
    fp.clipHighlights = false; // overlays never leak into the export readback
    fp.clipShadows = false;
    fp.adjustments = p;

    QImage result = core.renderOffscreen(buf, fp, QSize(cropW, cropH), QRhiTexture::RGBA32F);
    if (result.isNull())
        return {};

    // Scale to requested output dimensions while still in linear light —
    // gamma-space scaling darkens fine detail.
    if (result.width() != outW || result.height() != outH)
        result = result.scaled(outW, outH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    return result;
}

QImage renderClipSample(
    RendererCore& core,
    const ImageBuffer& buf,
    const GlobalAdjustment& p,
    bool clipHighlights,
    bool clipShadows) {
    if (!core.ready() || !buf.valid())
        return {};

    core.setCurveLut(curveLutRgba(p));

    // The on-screen display path (sRGB encode, monitor/proof LUT off) so the
    // clipping overlay actually runs — renderToImage uses the linear export
    // path where it is forced off. Used by the clipping golden test (adr/0009).
    const QRectF& cr = p.cropRect;
    int orientedW = buf.width;
    int orientedH = buf.height;
    if (orient::swapsAspect(p.orientation)) // oriented frame (docs/adr/0029)
        std::swap(orientedW, orientedH);
    const int cropW = (std::max) (1, int(cr.width() * orientedW + 0.5f));
    const int cropH = (std::max) (1, int(cr.height() * orientedH + 0.5f));

    RendererCore::FrameParams fp;
    fp.transform = QVector4D(1.0f, 1.0f, 0.0f, 0.0f);
    fp.cropRect = cr;
    fp.aspect = float(orientedW) / float(orientedH);
    fp.baseLook = true;
    fp.displayEncode = true;
    fp.curveInput = false;
    fp.useLut = false;
    fp.gamutWarn = false;
    fp.clipHighlights = clipHighlights;
    fp.clipShadows = clipShadows;
    fp.adjustments = p;

    return core.renderOffscreen(buf, fp, QSize(cropW, cropH), QRhiTexture::RGBA32F);
}

} // namespace offscreen
