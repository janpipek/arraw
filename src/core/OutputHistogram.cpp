#include "EncodedForOutput.h"
#include "OutputHistogramCount.h"
#include "TimingTrace.h"

#include <OutputHistogram.h>

#include <QImage>

#include <stdexcept>
#include <variant>

using namespace arraw;

OutputHistogram arraw::outputHistogram(const ImageBuffer& picture) {
    const auto* named = std::get_if<NamedEncoding>(&picture.encoding());
    if (named == nullptr || *named == workingEncoding || *named == perceptualEncoding) {
        throw std::invalid_argument(
            "An output histogram counts a picture in an output encoding; give the encoding to "
            "convert a developed render");
    }
    const detail::TimingSpan timing("histogram.output");
    return countOutputCodes(reinterpret_cast<const std::byte*>(picture.bytes().data()),
                            picture.size(), picture.rowStride(), picture.format(), *named);
}

OutputHistogram arraw::outputHistogram(const ImageBuffer& developed, NamedEncoding encoding) {
    if (encoding == workingEncoding || encoding == perceptualEncoding) {
        throw std::invalid_argument("The working and perceptual encodings are internal, not "
                                    "output ones");
    }
    // Only the working encoding is a developed render; the perceptual coordinate or an already
    // encoded picture would be converted as if it were linear.
    const auto* source = std::get_if<NamedEncoding>(&developed.encoding());
    if (source != nullptr && *source != workingEncoding) {
        throw std::invalid_argument(
            "Only a developed render, in the working encoding, can be converted to an output "
            "encoding; count a picture already in one with the other overload");
    }
    const detail::TimingSpan timing("histogram.output");
    const QImage converted = encodedForOutput(developed, encoding, QImage::Format_RGBA8888);
    return countOutputCodes(reinterpret_cast<const std::byte*>(converted.constBits()),
                            {static_cast<std::uint32_t>(converted.width()),
                             static_cast<std::uint32_t>(converted.height())},
                            static_cast<std::size_t>(converted.bytesPerLine()), PixelFormat::RgbaU8,
                            encoding);
}
