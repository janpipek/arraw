#include "CurveHistogram.h"

#include "ColorSpaces.h"
#include "ProcessingPlan.h"
#include "SampleConversion.h"
#include "TimingTrace.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <variant>

using namespace arraw;

namespace {

/// @brief Gives the bin a value in the perceptual coordinate falls in.
///
/// At or below 0, and NaN, is the first bin, as the curves take NaN for black;
/// at or above 1 the last.
std::size_t binOf(float value) {
    constexpr auto bins = static_cast<float>(curveHistogramBins);
    if (!(value > 0.0F)) {
        return 0;
    }
    if (!(value < 1.0F)) {
        return curveHistogramBins - 1;
    }
    // value * bins can round up to bins for a value just under 1.
    const auto bin = static_cast<std::size_t>(value * bins);
    return bin < curveHistogramBins ? bin : curveHistogramBins - 1;
}

/// @brief Counts every pixel of one sample layout.
template <typename Sample> void countSamples(const ImageBuffer& image, CurveHistogram& histogram) {
    const auto samples = image.samples<Sample>();
    const std::size_t channels = channelCount(image.format());
    const auto pixels = static_cast<std::size_t>(image.size().pixelCount());
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const Sample* in = &samples[pixel * channels];
        // Fully transparent pixels are not part of the picture; NaN alpha is
        // not visible either.
        if (channels == 4 && !(toUnit(in[3]) > 0.0F)) {
            continue;
        }
        const float red = toUnit(in[0]);
        const float green = toUnit(in[1]);
        const float blue = toUnit(in[2]);
        const float luminance = colorspaces::workingLuminance[0] * fromPerceptualSigned(red) +
                                colorspaces::workingLuminance[1] * fromPerceptualSigned(green) +
                                colorspaces::workingLuminance[2] * fromPerceptualSigned(blue);
        // Negative luminance and NaN fail the comparison and go to the first bin.
        const float luma = luminance > 0.0F ? toPerceptual(luminance) : 0.0F;
        ++histogram.luma[binOf(luma)];
        ++histogram.red[binOf(red)];
        ++histogram.green[binOf(green)];
        ++histogram.blue[binOf(blue)];
        ++histogram.pixels;
    }
}

} // namespace

CurveHistogram arraw::curveHistogram(const ImageBuffer& curveInput) {
    const auto* named = std::get_if<NamedEncoding>(&curveInput.encoding());
    if (named == nullptr || *named != perceptualEncoding) {
        throw std::invalid_argument(
            "A curve histogram counts samples in the perceptual encoding, as sample() gives them");
    }
    const detail::TimingSpan timing("histogram.curve");
    CurveHistogram histogram;
    switch (curveInput.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        countSamples<std::uint8_t>(curveInput, histogram);
        break;
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        countSamples<std::uint16_t>(curveInput, histogram);
        break;
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        countSamples<float>(curveInput, histogram);
        break;
    }
    return histogram;
}

CurveHistogram arraw::curveHistogram(const ImageBuffer& source, const DevelopState& state,
                                     const RenderRequest& request) {
    RenderRequest bilinear = request;
    bilinear.filter = ResizeFilter::Bilinear;
    return curveHistogram(sample(source, state, Tap::CurveInput, bilinear));
}
