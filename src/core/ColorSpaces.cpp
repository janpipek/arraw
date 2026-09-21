#include "ColorSpaces.h"

#include <stdexcept>

QColorSpace arraw::colorSpaceFor(ColorEncoding encoding) {
    switch (encoding) {
    case ColorEncoding::LinearRec2020:
        // Linear Rec.2020: real primaries containing both output gamuts, and a
        // linear transfer so exposure and blending are physically meaningful.
        return QColorSpace(QColorSpace::Primaries::Bt2020, QColorSpace::TransferFunction::Linear);
    case ColorEncoding::Srgb:
        return QColorSpace::SRgb;
    case ColorEncoding::DisplayP3:
        return QColorSpace::DisplayP3;
    case ColorEncoding::AdobeRgb:
        return QColorSpace::AdobeRgb;
    }
    throw std::invalid_argument("Unknown colour encoding");
}
