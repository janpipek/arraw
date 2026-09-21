#include "ImageCompare.h"

#include <QColorSpace>
#include <QFile>

#include <algorithm>
#include <cstdlib>
#include <ostream>
#include <stdexcept>

namespace arraw::test {

std::ostream& operator<<(std::ostream& stream, const Rgba8& colour) {
    return stream << '(' << colour.red << ", " << colour.green << ", " << colour.blue << ", "
                  << colour.alpha << ')';
}

double ImageDifference::exactFraction() const {
    return pixelCount == 0 ? 1.0
                           : static_cast<double>(exactPixels) / static_cast<double>(pixelCount);
}

std::ostream& operator<<(std::ostream& stream, const ImageDifference& difference) {
    return stream << "max " << difference.maxAbsDiff << " codes at (" << difference.worstX << ", "
                  << difference.worstY << "): expected " << difference.worstExpected << " got "
                  << difference.worstActual << "; mean " << difference.meanAbsDiff << "; "
                  << difference.exactPixels << '/' << difference.pixelCount << " pixels exact";
}

QImage decodeAsSrgb8(const std::filesystem::path& path) {
    QImage decoded(QFile(path).fileName());
    if (decoded.isNull()) {
        throw std::runtime_error("cannot decode " + path.string());
    }

    // An embedded profile is honoured so that a file exported in a wider space
    // is still comparable; both sides of the round-trip test are sRGB, where
    // this is a no-op.
    if (decoded.colorSpace().isValid()) {
        decoded.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    }
    return decoded.convertToFormat(QImage::Format_RGBA8888);
}

bool within(const Rgba8& actual, const Rgba8& expected, int tolerance) {
    return std::abs(actual.red - expected.red) <= tolerance &&
           std::abs(actual.green - expected.green) <= tolerance &&
           std::abs(actual.blue - expected.blue) <= tolerance &&
           std::abs(actual.alpha - expected.alpha) <= tolerance;
}

Rgba8 pixelAt(const QImage& image, int x, int y) {
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        throw std::out_of_range("pixel coordinates outside the image");
    }
    const auto* pixel = image.constScanLine(y) + static_cast<std::ptrdiff_t>(x) * 4;
    return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

ImageDifference compare(const QImage& expected, const QImage& actual) {
    if (expected.isNull() || actual.isNull()) {
        throw std::invalid_argument("cannot compare a null image");
    }
    if (expected.size() != actual.size()) {
        throw std::invalid_argument("cannot compare images of different sizes");
    }

    ImageDifference difference;
    difference.pixelCount = static_cast<std::uint64_t>(expected.width()) * expected.height();

    std::uint64_t total = 0;
    for (int y = 0; y < expected.height(); ++y) {
        const auto* expectedRow = expected.constScanLine(y);
        const auto* actualRow = actual.constScanLine(y);

        for (int x = 0; x < expected.width(); ++x) {
            const auto base = static_cast<std::ptrdiff_t>(x) * 4;
            int worstChannel = 0;
            for (int channel = 0; channel < 4; ++channel) {
                const int delta = std::abs(static_cast<int>(expectedRow[base + channel]) -
                                           static_cast<int>(actualRow[base + channel]));
                total += static_cast<std::uint64_t>(delta);
                worstChannel = std::max(worstChannel, delta);
            }

            if (worstChannel == 0) {
                ++difference.exactPixels;
            }
            if (worstChannel > difference.maxAbsDiff) {
                difference.maxAbsDiff = worstChannel;
                difference.worstX = x;
                difference.worstY = y;
                difference.worstExpected = pixelAt(expected, x, y);
                difference.worstActual = pixelAt(actual, x, y);
            }
        }
    }

    if (difference.pixelCount > 0) {
        difference.meanAbsDiff =
            static_cast<double>(total) / static_cast<double>(difference.pixelCount * 4);
    }
    return difference;
}

} // namespace arraw::test
