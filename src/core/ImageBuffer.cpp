#include "ImageBuffer.h"

#include "RowBands.h"

#include <algorithm>
#include <cmath>

using namespace std;
using namespace arraw;

constexpr static ImageSize validateSize(const ImageSize& size);

static constexpr size_t checkedMultiply(std::size_t left, std::size_t right);

static constexpr std::size_t checkedPixelCount(ImageSize size);

static constexpr std::size_t checkedSampleCount(ImageSize size, PixelFormat format);

static ImageBuffer::Storage allocateStorage(ImageSize size, std::size_t sampleCount,
                                            PixelFormat format);

ImageBuffer::ImageBuffer(ImageSize size, PixelFormat format, ColorEncoding encoding,
                         ImageOrientation orientation)
    : size_(validateSize(size)), format_(format), encoding_(std::move(encoding)),
      orientation_(orientation), rowStride_(checkedMultiply(size_.width, bytesPerPixel(format_))),
      storage_(allocateStorage(size_, checkedSampleCount(size_, format_), format_)) {}

span<byte> ImageBuffer::bytes() noexcept {
    return std::visit([](auto& samples) { return std::as_writable_bytes(std::span{samples}); },
                      storage_);
}

span<const byte> ImageBuffer::bytes() const noexcept {
    return std::visit([](const auto& samples) { return std::as_bytes(std::span{samples}); },
                      storage_);
}

size_t ImageBuffer::byteSize() const noexcept {
    return std::visit(
        [](const auto& samples) {
            using Sample = typename std::decay_t<decltype(samples)>::value_type;
            return samples.size() * sizeof(Sample);
        },
        storage_);
}

void ImageBuffer::setPixelScale(double scale) {
    if (!std::isfinite(scale) || !(scale > 0.0)) {
        throw std::invalid_argument("a pixel scale must be finite and above zero");
    }
    pixelScale_ = scale;
}

ImageBuffer ImageBuffer::clone() const {
    return *this;
}

/// @brief Allocates samples and zeroes them across threads, in bands of rows.
///
/// Not ::arraw::detail::forEachRowBand: zeroing is not a unit of any operation's
/// progress. The first touch of each page happens on the thread that zeroes it.
template <typename T>
static detail::SampleVector<T> zeroedSamples(ImageSize size, std::size_t sampleCount) {
    detail::SampleVector<T> samples(sampleCount);
    const std::size_t rowSamples = sampleCount / size.height;
    T* const data = samples.data();
    detail::splitRowBands(size.height, size.width,
                          [data, rowSamples](std::uint32_t first, std::uint32_t last, bool) {
                              std::fill(data + first * rowSamples, data + last * rowSamples, T{});
                          });
    return samples;
}

static ImageBuffer::Storage allocateStorage(ImageSize size, std::size_t sampleCount,
                                            PixelFormat format) {
    switch (format) {
    case PixelFormat::RgbU8:
    case PixelFormat::RgbaU8:
        return zeroedSamples<std::uint8_t>(size, sampleCount);
    case PixelFormat::RgbU16:
    case PixelFormat::RgbaU16:
        return zeroedSamples<std::uint16_t>(size, sampleCount);
    case PixelFormat::RgbF32:
    case PixelFormat::RgbaF32:
        return zeroedSamples<float>(size, sampleCount);
    }
    throw std::invalid_argument("unknown pixel format");
}

constexpr ImageSize validateSize(const ImageSize& size) {
    if (size.empty())
        throw std::invalid_argument("image dimensions must be non-zero");
    return size;
}

constexpr ::size_t checkedMultiply(const std::size_t left, const std::size_t right) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right)
        throw std::length_error("image buffer size overflows size_t");
    return left * right;
}

constexpr std::size_t checkedPixelCount(ImageSize size) {
    const auto count = size.pixelCount();
    if (count > std::numeric_limits<std::size_t>::max())
        throw std::length_error("image pixel count exceeds size_t");
    return static_cast<std::size_t>(count);
}

constexpr std::size_t checkedSampleCount(ImageSize size, PixelFormat format) {
    return checkedMultiply(checkedPixelCount(size), channelCount(format));
}
