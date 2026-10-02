#include "DisplayImage.h"

#include <Develop.h>

#include <QColorSpace>

#include <limits>
#include <stdexcept>

namespace arraw::app {

QImage toDisplayImage(const ImageBuffer& developed) {
    if (!isWorkingEncoding(developed.encoding()) || developed.format() != workingFormat) {
        throw std::invalid_argument("Only a developed image in the working encoding can be shown");
    }
    const ImageSize size = developed.size();
    constexpr auto qtLimit = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    if (size.width > qtLimit || size.height > qtLimit ||
        developed.rowStride() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::invalid_argument("Image too large to display");
    }

    // A view over the buffer's samples, not a copy: it must not outlive this
    // function. unsigned char may alias any object, so reading std::byte
    // storage through it is allowed.
    QImage view(reinterpret_cast<const uchar*>(developed.bytes().data()),
                static_cast<int>(size.width), static_cast<int>(size.height),
                static_cast<qsizetype>(developed.rowStride()), QImage::Format_RGBA32FPx4);
    // The same mapping as src/core/QtImage.cpp gives the working encoding.
    view.setColorSpace(
        QColorSpace(QColorSpace::Primaries::Bt2020, QColorSpace::TransferFunction::Linear));

    // Returns a new image with its own pixels, which is what lets it outlive
    // the view and the buffer.
    return view.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_RGBA8888);
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
    QImage image = toDisplayImage(develop(decoded, state, previewRequest(viewport)));
    image.setDevicePixelRatio(devicePixelRatio);
    return image;
}

} // namespace arraw::app
