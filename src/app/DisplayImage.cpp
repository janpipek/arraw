#include "DisplayImage.h"

#include "EncodedForOutput.h"
#include "TimingTrace.h"

#include <Develop.h>

#include <stdexcept>

namespace arraw::app {

QImage toDisplayImage(const ImageBuffer& developed) {
    const detail::TimingSpan timing("display.convert");
    if (!isWorkingEncoding(developed.encoding()) || developed.format() != workingFormat) {
        throw std::invalid_argument("Only a developed image in the working encoding can be shown");
    }
    // The conversion an 8-bit export makes, so that the window shows what would be written
    // (ADR 057). The result owns its pixels, so it outlives the buffer.
    return encodedForOutput(developed, NamedEncoding::Srgb, QImage::Format_RGBA8888);
}

RenderRequest previewRequest(QSize viewport) {
    if (viewport.isEmpty()) {
        throw std::invalid_argument("Cannot fit an image inside an empty viewport");
    }
    return RenderRequest{
        .size = RenderRequest::FitInside{static_cast<std::uint32_t>(viewport.width()),
                                         static_cast<std::uint32_t>(viewport.height())},
        .upscale = Upscale::Never};
}

QImage renderForViewport(const ImageBuffer& decoded, const DevelopState& state, QSize viewport,
                         qreal devicePixelRatio) {
    QImage image =
        toDisplayImage(develop(decoded, state, LensCorrection{}, previewRequest(viewport)));
    image.setDevicePixelRatio(devicePixelRatio);
    return image;
}

} // namespace arraw::app
