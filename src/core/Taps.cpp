#include "Taps.h"

#include "ProcessingPlan.h"

#include <cstddef>
#include <stdexcept>

using namespace arraw;

NamedEncoding arraw::tapEncoding(Tap tap) {
    switch (tap) {
    case Tap::CurveInput:
        return perceptualEncoding;
    }
    throw std::invalid_argument("A sample needs a recognised tap");
}

ImageBuffer arraw::encodeTap(const ImageBuffer& linear, Tap tap) {
    const NamedEncoding encoding = tapEncoding(tap);
    if (linear.format() != workingFormat || !isWorkingEncoding(linear.encoding())) {
        throw std::invalid_argument("A tap is encoded from working-format pixels in the working "
                                    "encoding");
    }
    ImageBuffer encoded(linear.size(), workingFormat, encoding, linear.orientation());
    encoded.setPixelScale(linear.pixelScale());
    const auto in = linear.samples<float>();
    const auto out = encoded.samples<float>();
    for (std::size_t index = 0; index < in.size(); index += 4) {
        out[index] = toPerceptualSigned(in[index]);
        out[index + 1] = toPerceptualSigned(in[index + 1]);
        out[index + 2] = toPerceptualSigned(in[index + 2]);
        out[index + 3] = in[index + 3];
    }
    return encoded;
}
