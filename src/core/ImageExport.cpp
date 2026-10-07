#include "ImageExport.h"

#include "MetadataEmbedding.h"
#include "QtImage.h"
#include "RowBands.h"
#include "TimingTrace.h"

#include <QBuffer>
#include <QByteArray>
#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using namespace arraw;

namespace {

/// @brief Resolves the format to encode, preferring an explicit request over
/// the destination's extension.
/// @param path Destination path, whose extension is the fallback.
/// @param options Export options, which may name a format outright.
/// @return The resolved file format.
/// @throws std::invalid_argument if no format was requested and the extension
/// is not one arraw writes.
ImageFileFormat extractImageFileFormat(const std::filesystem::path& path,
                                       const ExportOptions& options) {
    if (options.format.has_value()) {
        return *options.format;
    }

    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if (extension == ".jpg" || extension == ".jpeg") {
        return ImageFileFormat::Jpeg;
    }
    if (extension == ".png") {
        return ImageFileFormat::Png;
    }
    if (extension == ".tif" || extension == ".tiff") {
        return ImageFileFormat::Tiff;
    }

    throw std::invalid_argument(
        extension.empty() ? "no image format requested and the destination has no extension"
                          : "no image format requested and '" + extension + "' names none");
}

QByteArray fileFormatToString(ImageFileFormat format) {
    switch (format) {
    case ImageFileFormat::Png:
        return "png";
    case ImageFileFormat::Jpeg:
        return "jpg";
    case ImageFileFormat::Tiff:
        return "tiff";
    }
    throw std::invalid_argument("Unknown image file format");
}

/// @brief Validates the options applicable to the destination format.
void validateExportOptions(ImageFileFormat format, const ExportOptions& options) {
    if (options.encoding == workingEncoding || options.encoding == perceptualEncoding) {
        throw std::invalid_argument("The working and perceptual encodings are internal, not "
                                    "output ones");
    }
    if (options.bitDepth != 8 && options.bitDepth != 16) {
        throw std::invalid_argument("Export bit depth must be 8 or 16");
    }
    if (options.sharpening < 0 || options.sharpening > 100) {
        throw std::invalid_argument("Export sharpening must be between 0 and 100");
    }
    if (format == ImageFileFormat::Jpeg) {
        if (options.bitDepth != 8) {
            throw std::invalid_argument("JPEG export requires 8-bit samples");
        }
        if (options.quality < 0 || options.quality > 100) {
            throw std::invalid_argument("JPEG quality must be between 0 and 100");
        }
    }
}

/// @brief Checks whether every alpha sample in an RGBA buffer is opaque.
template <typename Sample> bool hasOpaqueAlpha(std::span<const Sample> samples, Sample opaque) {
    for (std::size_t index = 3; index < samples.size(); index += 4) {
        if (samples[index] != opaque) {
            return false;
        }
    }
    return true;
}

/// @brief Checks opacity before any conversion can quantise away transparency.
bool isOpaque(const ImageBuffer& image) {
    switch (image.format()) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbU16:
    case PixelFormat::RgbF32:
        return true;
    case PixelFormat::RgbaU8:
        return hasOpaqueAlpha(image.samples<std::uint8_t>(), std::uint8_t{255});
    case PixelFormat::RgbaU16:
        return hasOpaqueAlpha(image.samples<std::uint16_t>(), std::uint16_t{65535});
    case PixelFormat::RgbaF32:
        return hasOpaqueAlpha(image.samples<float>(), 1.0F);
    }
    throw std::invalid_argument("Unknown pixel format");
}

/// @brief Selects the encoder's sample layout, preserving alpha for PNG and TIFF.
QImage::Format outputPixelFormat(ImageFileFormat format, int bitDepth, bool hasAlpha) {
    if (format == ImageFileFormat::Jpeg) {
        return QImage::Format_RGB888;
    }
    if (bitDepth == 16) {
        return hasAlpha ? QImage::Format_RGBA64 : QImage::Format_RGBX64;
    }
    return hasAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
}

/// Gaussian sigma of the output sharpening, in output pixels. One pixel gives a
/// fine, print-and-screen-neutral halo that survives JPEG; larger radii would
/// need to know the output size and viewing distance, which export does not.
constexpr double sharpenSigma = 1.0;

/// Unsharp-mask strength at an amount of 100. A strength of 1.5 adds one and a
/// half times the detail (v - blur) back: a clearly crisp but not haloed result
/// on perceptually encoded values, where 1.0 is barely visible on downsized
/// output and 2+ rings visibly on hard edges.
constexpr float maxSharpenStrength = 1.5F;

/// @brief Builds the normalised one-sided-radius Gaussian kernel for the sharpening.
std::vector<float> sharpenKernel() {
    const int radius = static_cast<int>(std::ceil(3.0 * sharpenSigma));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    double total = 0.0;
    for (int offset = -radius; offset <= radius; ++offset) {
        const double weight = std::exp(-0.5 * offset * offset / (sharpenSigma * sharpenSigma));
        kernel[static_cast<std::size_t>(offset + radius)] = static_cast<float>(weight);
        total += weight;
    }
    for (float& weight : kernel) {
        weight = static_cast<float>(weight / total);
    }
    return kernel;
}

/// @brief Runs a function over bands of rows on every thread, without counting them as progress.
///
/// Each output row of the sharpening depends on inputs alone, so any split of the
/// rows gives the bits of the single-threaded loop (ADR 039, ADR 043).
/// @param rows Rows to cover.
/// @param width Pixels per row.
/// @param body Callable `(int first, int last)`, `last` exclusive.
template <typename Body> void forEachBand(int rows, int width, Body&& body) {
    detail::splitRowBands(static_cast<std::uint32_t>(rows), static_cast<std::uint32_t>(width),
                          [&body](std::uint32_t first, std::uint32_t last, bool /*onCaller*/) {
                              body(static_cast<int>(first), static_cast<int>(last));
                          });
}

/// @brief Blurs the colour channels of a premultiplied RGBA float image along one axis.
/// @param source Premultiplied samples, four floats per pixel, row-major.
/// @param width Pixels per row.
/// @param height Number of rows.
/// @param horizontal Whether to blur along rows rather than columns.
/// @param kernel Odd-length normalised weights; edges clamp.
/// @return The blurred samples; alpha is copied unchanged.
std::vector<float> blurAxis(const std::vector<float>& source, int width, int height,
                            bool horizontal, const std::vector<float>& kernel) {
    std::vector<float> result(source);
    const int radius = static_cast<int>(kernel.size() / 2);
    const int length = horizontal ? width : height;
    forEachBand(height, width, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            for (int x = 0; x < width; ++x) {
                const int along = horizontal ? x : y;
                std::array<float, 3> sum{};
                for (int tap = -radius; tap <= radius; ++tap) {
                    const int at = std::clamp(along + tap, 0, length - 1);
                    const std::size_t index =
                        4 * (horizontal ? static_cast<std::size_t>(y) * width + at
                                        : static_cast<std::size_t>(at) * width + x);
                    const float weight = kernel[static_cast<std::size_t>(tap + radius)];
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        sum[channel] += weight * source[index + channel];
                    }
                }
                const std::size_t out = 4 * (static_cast<std::size_t>(y) * width + x);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    result[out + channel] = sum[channel];
                }
            }
        }
    });
    return result;
}

/// @brief Applies an unsharp mask to an output-encoded float image.
/// @param image Non-premultiplied `QImage::Format_RGBA32FPx4`, modified in place.
/// @param amount Sharpening amount, 1–100.
void sharpenInPlace(QImage& image, int amount) {
    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<float> original(pixels * 4);
    forEachBand(height, width, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            const auto* line = reinterpret_cast<const float*>(image.constScanLine(y));
            float* row = original.data() + static_cast<std::size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                const float alpha = std::clamp(line[4 * x + 3], 0.0F, 1.0F);
                for (int channel = 0; channel < 3; ++channel) {
                    row[4 * x + channel] = line[4 * x + channel] * alpha;
                }
                row[4 * x + 3] = alpha;
            }
        }
    });

    const auto kernel = sharpenKernel();
    const auto blurred =
        blurAxis(blurAxis(original, width, height, true, kernel), width, height, false, kernel);
    const float strength = static_cast<float>(amount) / 100.0F * maxSharpenStrength;
    // Taken once, here: a scanLine call from a band would detach the image there.
    uchar* const bits = image.bits();
    const auto stride = static_cast<std::size_t>(image.bytesPerLine());
    forEachBand(height, width, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            auto* line = reinterpret_cast<float*>(bits + static_cast<std::size_t>(y) * stride);
            for (int x = 0; x < width; ++x) {
                const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
                const float alpha = original[index + 3];
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float value = original[index + channel];
                    const float sharpened = value + strength * (value - blurred[index + channel]);
                    const float clamped = std::clamp(sharpened, 0.0F, alpha);
                    line[4 * x + channel] = alpha > 0.0F ? clamped / alpha : 0.0F;
                }
            }
        }
    });
}

/// @brief Converts colour and sample depth, and selects profile metadata for export.
QImage prepareExportImage(const ImageBuffer& image, ImageFileFormat format,
                          const ExportOptions& options) {
    // A camera-native buffer has no colour space Qt could name, and no output
    // profile could describe one sensor's primaries to a viewer. Refusing here
    // says which stage is missing rather than which enumerator is unknown.
    const auto* sourceEncoding = std::get_if<NamedEncoding>(&image.encoding());
    if (sourceEncoding == nullptr) {
        throw std::invalid_argument(
            "A camera-native buffer must pass white balance before it can be exported");
    }
    const auto sourceSpace = qtimage::toColorSpace(*sourceEncoding);
    const auto targetSpace = qtimage::toColorSpace(options.encoding);
    QImage source = qtimage::toImage(image);
    if (source.isNull()) {
        throw std::invalid_argument("Unsupported or invalid image buffer");
    }
    if (format == ImageFileFormat::Jpeg && !isOpaque(image)) {
        throw std::invalid_argument("JPEG export requires opaque pixels");
    }

    source.setColorSpace(sourceSpace);
    const auto outputFormat = outputPixelFormat(format, options.bitDepth, source.hasAlphaChannel());
    QImage prepared;
    if (options.sharpening > 0) {
        // Sharpen the output-encoded values in float, before they are quantised.
        QImage floats = source.convertedToColorSpace(targetSpace, QImage::Format_RGBA32FPx4);
        if (floats.isNull()) {
            throw std::runtime_error("Cannot prepare image for export");
        }
        sharpenInPlace(floats, options.sharpening);
        prepared = floats.convertedTo(outputFormat);
    } else {
        prepared = source.convertedToColorSpace(targetSpace, outputFormat);
    }
    if (prepared.isNull()) {
        throw std::runtime_error("Cannot prepare image for export");
    }
    if (!options.embedProfile) {
        prepared.setColorSpace(QColorSpace{});
    }
    return prepared;
}

/// @brief Encodes a prepared image to a device.
void encode(QIODevice& device, const QImage& image, ImageFileFormat format,
            const ExportOptions& options) {
    QImageWriter writer(&device, fileFormatToString(format));
    if (format == ImageFileFormat::Jpeg) {
        writer.setQuality(options.quality);
    }
    if (!writer.write(image)) {
        throw std::runtime_error("Cannot encode image: " + writer.errorString().toStdString());
    }
}

} // namespace

void arraw::exportImage(const ImageBuffer& image, const std::filesystem::path& path,
                        const ExportOptions& options, const std::optional<ExportMetadata>& metadata,
                        DiagnosticLog& log) {
    const detail::TimingSpan timing("export.write");
    const auto format = extractImageFileFormat(path, options);
    validateExportOptions(format, options);
    const QImage qImage = prepareExportImage(image, format, options);

    /// Use QFile for path conversion on Qt 6.10; QSaveFile's std::filesystem::path
    /// constructor is available from Qt 6.11.
    const auto fileName = QFile(path).fileName();
    QSaveFile output(fileName);
    if (!output.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("Cannot open export destination: " +
                                 output.errorString().toStdString());
    }

    if (metadata && carriesAnything(metadata->selection)) {
        // Encoded in memory and given its metadata there, so that a failure of
        // either leaves the destination as it was, and the file is committed once.
        QByteArray encoded;
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        encode(buffer, qImage, format, options);
        buffer.close();
        const QByteArray embedded = embedMetadata(
            encoded, *metadata,
            {qImage.width(), qImage.height(), options.encoding == NamedEncoding::Srgb}, log);
        const QByteArray& bytes = embedded.isNull() ? encoded : embedded;
        if (output.write(bytes) != bytes.size()) {
            throw std::runtime_error("Cannot write export: " + output.errorString().toStdString());
        }
    } else {
        encode(output, qImage, format, options);
    }

    if (!output.commit()) {
        throw std::runtime_error("Cannot commit export: " + output.errorString().toStdString());
    }
}

bool arraw::isSameFile(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code error;
    if (std::filesystem::exists(a, error) && std::filesystem::exists(b, error)) {
        const bool same = std::filesystem::equivalent(a, b, error);
        if (!error) {
            return same;
        }
    }
    const auto normal = [](const std::filesystem::path& path) {
        std::error_code ignored;
        const auto absolute = std::filesystem::absolute(path, ignored);
        return (ignored ? path : absolute).lexically_normal();
    };
    return normal(a) == normal(b);
}
